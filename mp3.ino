#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DFRobotDFPlayerMini.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "images.h"

#define BTN_PLAY_PAUSE  18
#define BTN_NEXT        5
#define BTN_PREV        19
#define BTN_MODE        27
#define POT_VOLUME      34

#define SCREEN_W      128
#define SCREEN_H       64
#define OLED_RESET     -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

DFRobotDFPlayerMini dfPlayer;
WebServer           server(80);
DNSServer           dnsServer;
Preferences         prefs;

#define DEBOUNCE_MS           30
#define DOUBLE_CLICK_MS      350
#define VOL_READ_INTERVAL    100
#define ANIM_INTERVAL        120
#define CLOCK_INTERVAL      1000
#define EPOCH_SAVE_INTERVAL 60000UL
#define MARQUEE_INTERVAL     300
#define SD_RETRY_INTERVAL  10000UL
#define DEEP_SLEEP_WAKE_US  60000000ULL
#define DEEP_SLEEP_WAKE_S   60

#define C_WHITE  WHITE
#define C_BLACK  BLACK

#define AP_SSID  "MP3-Player"
#define AP_PASS  ""

#define TRACKS_FILE  "/tracks.json"

RTC_DATA_ATTR time_t rtcEpoch        = 0;
RTC_DATA_ATTR int    rtcCurrentTrack = 1;
RTC_DATA_ATTR int    rtcTotalTracks  = 0;
RTC_DATA_ATTR int    rtcVolume       = 15;
RTC_DATA_ATTR bool   rtcSdPresent    = false;

enum Screen { SCR_PLAYER, SCR_NO_SD, SCR_WIFI, SCR_OFF };
Screen currentScreen = SCR_PLAYER;

bool  isPlaying    = false;
float discAngle = 0.0;
bool  sdPresent    = false;
bool  lowPowerMode = false;
int   currentTrack = 1;
int   totalTracks  = 0;
int   volume       = 15;
int   lastVolRaw   = 0;

int trackScrollIdx = 0;
unsigned long lastScrollTick = 0;

unsigned long lastVolRead   = 0;
unsigned long lastAnimTick  = 0;
unsigned long lastClockTick = 0;
unsigned long lastEpochSave = 0;
unsigned long lastSDRetry   = 0;

const int8_t  BTN_PINS[4]         = { BTN_PLAY_PAUSE, BTN_NEXT, BTN_PREV, BTN_MODE };
bool          btnStableState[4]   = { HIGH, HIGH, HIGH, HIGH };
bool          btnRawStatePrev[4]  = { HIGH, HIGH, HIGH, HIGH };
unsigned long btnDebounceStart[4] = { 0, 0, 0, 0 };

bool          modeClickPending = false;
unsigned long modeClickTime    = 0;

#define RITMO_BARS   7
#define RITMO_MAX_H  10
int barHeight[RITMO_BARS] = {4,6,8,5,6,3,5};
int barDir[RITMO_BARS]    = {1,1,-1,1,-1,1,-1};
int barMaxH[RITMO_BARS]   = {8,9,10,8,9,6,8};
int barMinH[RITMO_BARS]   = {2,2,2,2,2,2,2};
int dispVolBar = 0;

time_t        bootEpoch = 0;
unsigned long bootMs    = 0;

time_t nowEpoch() {
  return bootEpoch + (time_t)((millis() - bootMs) / 1000UL);
}

void currentTimeStr(char* buf) {
  time_t t = nowEpoch();
  struct tm* tm_info = gmtime(&t);
  sprintf(buf, "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
}

void saveEpochToNVS(time_t epoch) {
  prefs.begin("mp3cfg", false);
  prefs.putULong("epoch", (unsigned long)epoch);
  prefs.end();
}

time_t loadEpochFromNVS() {
  prefs.begin("mp3cfg", true);
  unsigned long e = prefs.getULong("epoch", 0);
  prefs.end();
  return (time_t)e;
}

String getTrackName(int track) {
  if (!LittleFS.exists(TRACKS_FILE)) return "Track " + String(track);
  File f = LittleFS.open(TRACKS_FILE, "r");
  if (!f) return "Track " + String(track);
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return "Track " + String(track);
  String key = String(track);
  return doc.containsKey(key) ? doc[key].as<String>() : "Track " + String(track);
}

bool setTrackName(int track, const String& name) {
  DynamicJsonDocument doc(16384);
  if (LittleFS.exists(TRACKS_FILE)) {
    File f = LittleFS.open(TRACKS_FILE, "r");
    if (f) { deserializeJson(doc, f); f.close(); }
  }
  doc[String(track)] = name;
  File f = LittleFS.open(TRACKS_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool saveAllTrackNames(const String& jsonBody) {
  DynamicJsonDocument incoming(16384);
  if (deserializeJson(incoming, jsonBody)) return false;
  DynamicJsonDocument doc(16384);
  if (LittleFS.exists(TRACKS_FILE)) {
    File f = LittleFS.open(TRACKS_FILE, "r");
    if (f) { deserializeJson(doc, f); f.close(); }
  }
  for (JsonPair kv : incoming.as<JsonObject>()) {
    String val = kv.value().as<String>();
    val.trim();
    if (val.length() > 0) doc[kv.key()] = val;
  }
  File f = LittleFS.open(TRACKS_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

char trackName[32] = "Track 1";

void loadCurrentTrackName() {
  String n = getTrackName(currentTrack);
  n.toCharArray(trackName, 32);

  trackScrollIdx = 0;
  lastScrollTick = millis() - MARQUEE_INTERVAL;
}

void drawScreen1();
void drawScreen2();
void drawScreen3();
void updateRitmoAnim();
void updateVolBar();
void updateClockDisplay();
void setupWiFi();
void stopWiFi();
void autoNextTrack();
void updateTrackNameOnDisplay();
void checkButtons();
void btnPlayPause();
void btnNext();
void btnPrev();
void onModeButtonPress();
void modeSingleClickAction();
void enterLowPowerMode();
void goToDeepSleep();
void readVolume();
void handleWebRoot();
void handleWebSaveClock();
void handleWebSaveTracks();
void handleWebGetTracks();
void handleWebNotFound();
void updateTrackMarquee();
bool initDFPlayer();

bool initDFPlayer() {
  bool dfOk = false;

  for (int tentativa = 1; tentativa <= 3 && !dfOk; tentativa++) {
    Serial.printf("[DFPlayer] tentativa %d/3...\n", tentativa);

    dfOk = dfPlayer.begin(Serial2, false, true);
    if (!dfOk) delay(3000);
  }

  if (!dfOk) {
    Serial.println("[DFPlayer] begin() falhou em todas as tentativas.");
    return false;
  }

  delay(300);

  if (dfPlayer.available()) {
    uint8_t type = dfPlayer.readType();
    int      val  = dfPlayer.read();
    if (type == DFPlayerCardRemoved || (type == DFPlayerError && val == Busy)) {
      Serial.println("[DFPlayer] Modulo avisou: sem cartao SD.");
      return false;
    }
  }

  int tracks = dfPlayer.readFileCounts();
  Serial.printf("[DFPlayer] totalTracks (1a leitura): %d\n", tracks);

  for (int i = 1; i <= 4 && tracks <= 0; i++) {
    delay(500);
    tracks = dfPlayer.readFileCounts();
    Serial.printf("[DFPlayer] totalTracks (%da releitura): %d\n", i + 1, tracks);
  }

  if (tracks <= 0) return false;

  totalTracks = tracks;
  return true;
}

void goToDeepSleep() {
  rtc_gpio_pullup_en((gpio_num_t)BTN_MODE);
  rtc_gpio_pulldown_dis((gpio_num_t)BTN_MODE);
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_WAKE_US);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_MODE, 0);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  rtc_gpio_deinit((gpio_num_t)BTN_MODE);

  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    rtcEpoch += DEEP_SLEEP_WAKE_S;
    saveEpochToNVS(rtcEpoch);
    goToDeepSleep();
  }

  if (BTN_PLAY_PAUSE >= 0) pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
  if (BTN_NEXT       >= 0) pinMode(BTN_NEXT,       INPUT_PULLUP);
  if (BTN_PREV       >= 0) pinMode(BTN_PREV,       INPUT_PULLUP);
  if (BTN_MODE       >= 0) pinMode(BTN_MODE,       INPUT_PULLUP);

  for (int i = 0; i < 4; i++) {
    if (BTN_PINS[i] >= 0) {
      btnRawStatePrev[i] = digitalRead(BTN_PINS[i]);
      btnStableState[i]  = btnRawStatePrev[i];
    }
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: falha ao montar");
  }

  Wire.begin(32, 33);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 nao encontrado!");
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(C_WHITE);
  display.setTextWrap(false);
  display.setTextSize(1);
  display.display();

  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    bootEpoch    = rtcEpoch;
    bootMs       = millis();
    currentTrack = rtcCurrentTrack;
    totalTracks  = rtcTotalTracks;
    volume       = rtcVolume;
    sdPresent    = rtcSdPresent;
    lowPowerMode = false;

    Serial2.begin(9600, SERIAL_8N1, 16, 17);
    delay(50);

    if (sdPresent) {
      dfPlayer.begin(Serial2, false, false);
      delay(100);
      dfPlayer.volume(volume);
      loadCurrentTrackName();
      dfPlayer.start();
      isPlaying = true;
      currentScreen = SCR_PLAYER;
      drawScreen1();
    } else {
      currentScreen = SCR_NO_SD;
      drawScreen2();
    }

    lastAnimTick  = millis();
    lastClockTick = millis();
    lastVolRead   = millis();
    lastEpochSave = millis();
    return;
  }

  time_t saved = loadEpochFromNVS();
  bootEpoch = (saved > 1000000000UL) ? saved : 0;
  bootMs    = millis();

  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  delay(2500);

  sdPresent = initDFPlayer();
  if (sdPresent) {
    dfPlayer.volume(volume);
    delay(100);
    currentTrack = 1;
    loadCurrentTrackName();
    dfPlayer.playMp3Folder(currentTrack);
    isPlaying = true;
    Serial.printf("[DFPlayer] SD OK – %d faixas – tocando #1\n", totalTracks);
  } else {
    Serial.println("[DFPlayer] SD nao encontrado ou sem faixas.");
  }

  if (!sdPresent) {
    currentScreen = SCR_NO_SD;
    drawScreen2();
  } else {
    currentScreen = SCR_PLAYER;
    drawScreen1();
  }
}

void loop() {

  checkButtons();

  if (modeClickPending && (millis() - modeClickTime) > DOUBLE_CLICK_MS) {
    modeClickPending = false;
    modeSingleClickAction();
  }

  if (!sdPresent && currentScreen == SCR_NO_SD && millis() - lastSDRetry > SD_RETRY_INTERVAL) {
    lastSDRetry = millis();
    Serial.println("[DFPlayer] Tentando detectar o SD novamente...");
    if (initDFPlayer()) {
      sdPresent = true;
      dfPlayer.volume(volume);
      delay(100);
      currentTrack = 1;
      loadCurrentTrackName();
      dfPlayer.playMp3Folder(currentTrack);
      isPlaying = true;
      currentScreen = SCR_PLAYER;
      drawScreen1();
      Serial.printf("[DFPlayer] SD detectado – %d faixas – tocando #1\n", totalTracks);
    }
  }

  static unsigned long lastTrackEnd = 0;
  if (sdPresent && currentScreen == SCR_PLAYER && dfPlayer.available()) {
    uint8_t tipo = dfPlayer.readType();

    if (tipo == DFPlayerPlayFinished) {
      if (millis() - lastTrackEnd > 1000) {
        lastTrackEnd = millis();
        Serial.printf("[DFPlayer] Faixa %d terminou → avancando.\n", currentTrack);
        autoNextTrack();
      }
    }

    if (tipo == DFPlayerError) {
      Serial.printf("[DFPlayer] Erro: %d\n", dfPlayer.read());
    }
  }

  if (currentScreen == SCR_PLAYER && millis()-lastVolRead > VOL_READ_INTERVAL) {
    lastVolRead = millis();
    readVolume();
  }

  if (currentScreen == SCR_PLAYER) {
    if (millis()-lastAnimTick > ANIM_INTERVAL) {
      lastAnimTick = millis();
      if (isPlaying) updateRitmoAnim();
      updateVolBar();
    }
    updateTrackMarquee();
  }

  if (currentScreen != SCR_WIFI && millis()-lastClockTick > CLOCK_INTERVAL) {
    lastClockTick = millis();
    updateClockDisplay();
  }

  if (bootEpoch > 0 && millis()-lastEpochSave > EPOCH_SAVE_INTERVAL) {
    lastEpochSave = millis();
    saveEpochToNVS(nowEpoch());
  }

  if (currentScreen == SCR_WIFI) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

void autoNextTrack() {
  currentTrack++;
  if (currentTrack > totalTracks) {
    currentTrack = 1;
    Serial.println("[DFPlayer] Fim da lista → voltando para faixa 1.");
  }
  loadCurrentTrackName();
  delay(100);
  dfPlayer.playMp3Folder(currentTrack);
  isPlaying = true;
  if (currentScreen == SCR_PLAYER) {
    updateTrackNameOnDisplay();

    display.fillRect(44, 46, 16, 14, C_BLACK);
    display.fillRect(46, 48, 3, 9, C_WHITE);
    display.fillRect(51, 48, 3, 9, C_WHITE);
    display.display();
  }
}

void updateTrackNameOnDisplay() {
  display.fillRect(23, 15, 54, 10, C_BLACK);
  display.display();
}

void checkButtons() {
  for (int i = 0; i < 4; i++) {
    if (BTN_PINS[i] < 0) continue;

    bool raw = digitalRead(BTN_PINS[i]);
    if (raw != btnRawStatePrev[i]) {
      btnDebounceStart[i] = millis();
      btnRawStatePrev[i]  = raw;
    }

    if ((millis() - btnDebounceStart[i]) > DEBOUNCE_MS) {
      if (raw != btnStableState[i]) {
        btnStableState[i] = raw;
        if (btnStableState[i] == LOW) {
          switch (i) {
            case 0: btnPlayPause();      break;
            case 1: btnNext();           break;
            case 2: btnPrev();           break;
            case 3: onModeButtonPress(); break;
          }
        }
      }
    }
  }
}

void btnPlayPause() {
  if (!sdPresent || currentScreen != SCR_PLAYER) return;
  if (isPlaying) { dfPlayer.pause(); isPlaying = false; }
  else           { dfPlayer.start(); isPlaying = true;  }

  display.fillRect(44, 46, 16, 14, C_BLACK);
  if (!isPlaying) {
    display.drawBitmap(45, 48, image_play_hover_bits, 10, 10, C_WHITE);
  } else {
    display.fillRect(46, 48, 3, 9, C_WHITE);
    display.fillRect(51, 48, 3, 9, C_WHITE);
  }
  display.display();
}

void btnNext() {
  if (!sdPresent || currentScreen != SCR_PLAYER) return;
  currentTrack++;
  if (currentTrack > totalTracks) currentTrack = 1;
  Serial.printf("[BTN_NEXT] currentTrack=%d totalTracks=%d\n", currentTrack, totalTracks);
  loadCurrentTrackName();
  delay(100);
  dfPlayer.playMp3Folder(currentTrack);
  isPlaying = true;
  updateTrackNameOnDisplay();

  display.fillRect(44, 46, 16, 14, C_BLACK);
  display.fillRect(46, 48, 3, 9, C_WHITE);
  display.fillRect(51, 48, 3, 9, C_WHITE);
  display.display();
}

void btnPrev() {
  if (!sdPresent || currentScreen != SCR_PLAYER) return;
  currentTrack--;
  if (currentTrack < 1) currentTrack = (totalTracks > 0) ? totalTracks : 1;
  Serial.printf("[BTN_PREV] currentTrack=%d totalTracks=%d\n", currentTrack, totalTracks);
  loadCurrentTrackName();
  delay(100);
  dfPlayer.playMp3Folder(currentTrack);
  isPlaying = true;
  updateTrackNameOnDisplay();
  display.fillRect(44, 46, 16, 14, C_BLACK);
  display.fillRect(46, 48, 3, 9, C_WHITE);
  display.fillRect(51, 48, 3, 9, C_WHITE);
  display.display();
}

void onModeButtonPress() {
  if (modeClickPending) {
    modeClickPending = false;
    enterLowPowerMode();
  } else {
    modeClickPending = true;
    modeClickTime    = millis();
  }
}

void modeSingleClickAction() {
  if (currentScreen != SCR_WIFI) {
    currentScreen = SCR_WIFI;
    if (isPlaying) { dfPlayer.pause(); isPlaying = false; }
    setupWiFi();
    drawScreen3();
  } else {
    stopWiFi();
    if (sdPresent) { currentScreen = SCR_PLAYER; drawScreen1(); }
    else           { currentScreen = SCR_NO_SD;  drawScreen2(); }
  }
}

void enterLowPowerMode() {
  Serial.println("[LowPower] Duplo clique detectado -> desligando.");
  if (currentScreen == SCR_WIFI) stopWiFi();

  if (isPlaying) {
    dfPlayer.pause();
    isPlaying = false;
  }

  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  time_t epochNow = nowEpoch();
  saveEpochToNVS(epochNow);

  rtcEpoch        = epochNow;
  rtcCurrentTrack = currentTrack;
  rtcTotalTracks  = totalTracks;
  rtcVolume       = volume;
  rtcSdPresent    = sdPresent;

  currentScreen = SCR_OFF;
  lowPowerMode  = true;

  goToDeepSleep();
}

void readVolume() {
  if (POT_VOLUME < 0) return;
  int raw = analogRead(POT_VOLUME);

  if (raw < 60) {
    raw = 0;
  } else {
    if (abs(raw - lastVolRaw) < 40) return;
  }
  lastVolRaw = raw;

  float norm = (float)raw / 3800.0f;
  if (norm > 1.0f) norm = 1.0f;
  if (norm < 0.0f) norm = 0.0f;

  float coeficiente = 0.6f;
  int newVol = 0;

  if (norm > 0.0f) {
    newVol = (int)(30.0f * powf(norm, coeficiente));
  }

  if (newVol > 30) newVol = 30;
  if (newVol < 0)  newVol = 0;

  if (newVol != volume) {
    volume = newVol;
    dfPlayer.volume(volume);
    Serial.printf("[Volume] Raw: %d | Escolhido: %d/30\n", raw, volume);
  }
}

void updateClockDisplay() {
  char buf[6];
  currentTimeStr(buf);
  display.fillRect(3, 0, 60, 11, C_BLACK);
  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print(buf);
  display.display();
}

void updateTrackMarquee() {
  const int clearX = 23, clearY = 15, clearW = 54, clearH = 10;
  const int textX   = 29, textY = 17;
  const int charW   = 6;
  const int maxChars = (clearX + clearW - textX) / charW;

  if (millis() - lastScrollTick < MARQUEE_INTERVAL) return;
  lastScrollTick = millis();

  String name = trackName;

  display.fillRect(clearX, clearY, clearW, clearH, C_BLACK);
  display.setCursor(textX, textY);
  display.setTextSize(1);
  display.setTextColor(C_WHITE);

  if ((int)name.length() <= maxChars) {
    display.print(name);
  }

  else {
    String paddedName = name + "    ";
    String visibleText = "";

    for (int i = 0; i < maxChars; i++) {
      int idx = (trackScrollIdx + i) % paddedName.length();
      visibleText += paddedName[idx];
    }
    display.print(visibleText);

    trackScrollIdx++;
    if (trackScrollIdx >= (int)paddedName.length()) {
      trackScrollIdx = 0;
    }
  }

  display.display();
}

void updateRitmoAnim() {
  const int baseX = 25, baseY = 40, barW = 5, gap = 2;
  for (int i = 0; i < RITMO_BARS; i++) {
    int x = baseX + i * (barW + gap);
    display.fillRect(x, baseY - barMaxH[i] - 1, barW, barMaxH[i] + 1, C_BLACK);
    barHeight[i] += barDir[i];
    if (barHeight[i] >= barMaxH[i]) { barHeight[i] = barMaxH[i]; barDir[i] = -1; }
    if (barHeight[i] <= barMinH[i]) { barHeight[i] = barMinH[i]; barDir[i] =  1; }
    if (random(8) == 0) barMaxH[i] = random(6, RITMO_MAX_H + 1);
    display.fillRect(x, baseY - barHeight[i], barW, barHeight[i], C_WHITE);
  }

  int xc = 100, yc = 37;
  int rMin = 5,  rMax = 18;

  int oldX1 = xc + (int)(rMin * sin(discAngle));
  int oldY1 = yc - (int)(rMin * cos(discAngle));
  int oldX2 = xc + (int)(rMax * sin(discAngle));
  int oldY2 = yc - (int)(rMax * cos(discAngle));
  display.drawLine(oldX1, oldY1, oldX2, oldY2, C_BLACK);

  discAngle += 0.25;
  if (discAngle >= 2 * PI) {
    discAngle -= 2 * PI;
  }

  int newX1 = xc + (int)(rMin * sin(discAngle));
  int newY1 = yc - (int)(rMin * cos(discAngle));
  int newX2 = xc + (int)(rMax * sin(discAngle));
  int newY2 = yc - (int)(rMax * cos(discAngle));
  display.drawLine(newX1, newY1, newX2, newY2, C_WHITE);

  display.drawCircle(xc, yc, 5, C_WHITE);
  display.drawCircle(xc, yc, 18, C_WHITE);

  display.display();
}

void updateVolBar() {
  const int barX = 6, barY = 24, barW = 5, barH = 29;
  int targetFill = map(volume, 0, 30, 0, barH - 2);
  if (dispVolBar == targetFill) return;
  if (dispVolBar < targetFill) dispVolBar++;
  else                         dispVolBar--;
  display.fillRect(barX+1, barY+1, barW-2, barH-2, C_BLACK);
  if (dispVolBar > 0)
    display.fillRect(barX+1, barY + barH - 1 - dispVolBar, barW-2, dispVolBar, C_WHITE);
  display.display();
}

void drawScreen1() {
  display.clearDisplay();

  display.drawLine(1, 12, 126, 12, C_WHITE);

  char buf[6]; currentTimeStr(buf);
  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print(buf);

  display.drawBitmap(112, 2, image_battery_charging_bits, 12, 8, C_WHITE);

  display.drawRect(6, 24, 5, 29, C_WHITE);
  dispVolBar = 0;

  display.drawBitmap(6, 55, image_Voldwn_bits, 6, 6, C_WHITE);
  display.drawBitmap(6, 16, image_Volup_bits,  8, 6, C_WHITE);

  display.drawRect(24, 28, 50, 12, C_WHITE);

  display.drawCircle(100, 37, 18, C_WHITE);
  display.drawCircle(100, 37,  5, C_WHITE);
  display.drawLine(100, 32, 100, 19, C_WHITE);

  discAngle = 0.0;

  display.fillTriangle(32,56, 32,48, 25,52, C_WHITE);
  display.fillRect(22, 48, 2, 9, C_WHITE);

  display.fillTriangle(66,56, 66,48, 73,52, C_WHITE);
  display.fillRect(75, 48, 2, 9, C_WHITE);

  if (!isPlaying) {
    display.drawBitmap(45, 48, image_play_hover_bits, 10, 10, C_WHITE);
  } else {
    display.fillRect(46, 48, 3, 9, C_WHITE);
    display.fillRect(51, 48, 3, 9, C_WHITE);
  }

  trackScrollIdx = 0;
  lastScrollTick = millis() - MARQUEE_INTERVAL;

  display.display();
}

void drawScreen2() {
  display.clearDisplay();
  display.drawLine(1, 12, 126, 12, C_WHITE);
  char buf[6]; currentTimeStr(buf);
  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print(buf);

  display.drawBitmap(112, 2, image_battery_charging_copy_1_bits, 12, 8, C_WHITE);
  display.drawBitmap(85, 17, image_SDQuestion_bits, 35, 43, C_WHITE);
  display.setCursor(8, 22); display.println("Nenhum");
  display.setCursor(8, 34); display.println("cartao SD");
  display.setCursor(8, 46); display.println("detectado!");

  display.display();
}

void drawScreen3() {
  display.clearDisplay();

  display.fillCircle(20, 42, 2, C_WHITE);
  display.drawCircle(20, 42,  7, C_WHITE);
  display.drawCircle(20, 42, 13, C_WHITE);
  display.drawCircle(20, 42, 19, C_WHITE);
  display.fillRect(0, 44, 42, 22, C_BLACK);

  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(45,  8); display.println("Modo WiFi");
  display.setCursor(45, 20); display.println("Conecte em:");
  display.setCursor(45, 30); display.println(AP_SSID);
  display.setCursor(45, 42); display.println("Acesse o site");
  display.setCursor(45, 52);
  display.println("que abrir");

  display.display();
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) > 0 ? AP_PASS : nullptr);

  server.on("/",            HTTP_GET,  handleWebRoot);
  server.on("/saveclock",   HTTP_POST, handleWebSaveClock);
  server.on("/savetracks",  HTTP_POST, handleWebSaveTracks);
  server.on("/gettracks",   HTTP_GET,  handleWebGetTracks);
  server.onNotFound(handleWebNotFound);
  server.begin();

  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.print("[WiFi] AP IP: "); Serial.println(WiFi.softAPIP());
}

void stopWiFi() {
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void handleWebRoot() {
  char timeBuf[6]; currentTimeStr(timeBuf);

  DynamicJsonDocument tracksDoc(16384);
  bool tracksDocOk = false;
  if (LittleFS.exists(TRACKS_FILE)) {
    File f = LittleFS.open(TRACKS_FILE, "r");
    if (f) {
      tracksDocOk = (deserializeJson(tracksDoc, f) == DeserializationError::Ok);
      f.close();
    }
  }

  String tracksJson = "{";
  for (int i = 1; i <= totalTracks; i++) {
    String key = String(i);
    String n = (tracksDocOk && tracksDoc.containsKey(key)) ? tracksDoc[key].as<String>() : ("Track " + key);
    n.replace("\"", "\\\"");
    tracksJson += "\"" + key + "\":\"" + n + "\"";
    if (i < totalTracks) tracksJson += ",";
  }
  tracksJson += "}";

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MP3 Player</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0f0f0f;color:#eee;padding:20px}
.card{background:#1a1a2e;border:1px solid #e94560;border-radius:16px;padding:28px;max-width:520px;margin:0 auto 24px;box-shadow:0 0 30px #e9456022}
h1{text-align:center;color:#e94560;font-size:1.3rem;margin-bottom:20px;letter-spacing:2px}
h2{color:#e94560;font-size:1rem;margin-bottom:14px;letter-spacing:1px}
.icon{text-align:center;font-size:2rem;margin-bottom:10px}
label{display:block;font-size:.75rem;color:#aaa;margin-bottom:4px;margin-top:14px;text-transform:uppercase;letter-spacing:1px}
input[type=text],input[type=time]{width:100%;background:#0f0f0f;border:1px solid #333;border-radius:8px;color:#eee;padding:9px 13px;font-size:.95rem;outline:none;transition:border .2s}
input[type=text]:focus,input[type=time]:focus{border-color:#e94560}
.hint{font-size:.7rem;color:#555;margin-top:3px}
.btn{display:inline-block;margin-top:16px;background:linear-gradient(135deg,#e94560,#c62a47);color:#fff;border:none;border-radius:9px;padding:11px 20px;font-size:.95rem;font-weight:600;cursor:pointer;letter-spacing:1px;transition:opacity .2s;width:100%}
.btn:hover{opacity:.85}
.btn-sec{background:linear-gradient(135deg,#2a4a8a,#1a2a6a)}
.status{display:none;text-align:center;margin-top:12px;padding:9px;border-radius:8px;font-size:.85rem}
.ok{background:#1b4332;color:#52b788;display:block}
.err{background:#4a1c1c;color:#e94560;display:block}
.track-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.track-num{color:#e94560;font-size:.8rem;min-width:32px;text-align:right;flex-shrink:0}
.track-input{flex:1}
#loading{text-align:center;color:#555;padding:20px;font-size:.9rem}
</style>
</head>
<body>
<div class="card">
  <div class="icon">🎵</div>
  <h1>MP3 Player</h1>
  <h2>⏰ Acertar Relógio</h2>
  <label>Hora atual</label>
  <input type="time" id="clockInput" value=")rawhtml" + String(timeBuf) + R"rawhtml(">
  <div class="hint">Sincroniza o horário exibido no display</div>
  <button class="btn" onclick="saveClock()">💾 Salvar Hora</button>
  <div class="status" id="stClock"></div>
</div>

<div class="card">
  <h2>🎵 Renomear Faixas (<span id="totalCount">)rawhtml" + String(totalTracks) + R"rawhtml(</span> no SD)</h2>
  <div id="loading">Carregando faixas...</div>
  <div id="trackList" style="display:none"></div>
  <button class="btn btn-sec" id="btnSaveTracks" style="display:none" onclick="saveTracks()">💾 Salvar Nomes</button>

  <div class="status" id="stTracks"></div>
</div>

<script>
const TRACKS = )rawhtml" + tracksJson + R"rawhtml(;
const total  = parseInt(document.getElementById('totalCount').textContent);

function renderTracks() {
  const list = document.getElementById('trackList');
  list.innerHTML = '';
  for (let i = 1; i <= total; i++) {
    const val = TRACKS[i] || ('Track ' + i);
    const row = document.createElement('div');
    row.className = 'track-row';
    row.innerHTML =
      '<span class="track-num">#' + i + '</span>' +
      '<input class="track-input" type="text" id="t' + i +
      '" maxlength="31" value="' + val.replace(/"/g,'&quot;') + '">';
    list.appendChild(row);
  }
  document.getElementById('loading').style.display = 'none';
  list.style.display = 'block';
  document.getElementById('btnSaveTracks').style.display = 'block';
}

async function saveClock() {
  const st = document.getElementById('stClock');
  const val = document.getElementById('clockInput').value;
  if (!val) { showStatus(st, false, 'Informe a hora!'); return; }
  const [h, m] = val.split(':').map(Number);
  const now = new Date();
  now.setUTCHours(h, m, 0, 0);
  const epoch = Math.floor(now.getTime() / 1000);
  const r = await fetch('/saveclock', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'epoch=' + epoch
  });
  showStatus(st, r.ok, r.ok ? '✅ Relógio atualizado!' : '❌ Erro ao salvar.');
}

async function saveTracks() {
  const st = document.getElementById('stTracks');
  const obj = {};
  for (let i = 1; i <= total; i++) {
    const el = document.getElementById('t' + i);
    if (el) obj[i] = el.value.trim() || ('Track ' + i);
  }
  const r = await fetch('/savetracks', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(obj)
  });
  showStatus(st, r.ok, r.ok ? '✅ Nomes salvos!' : '❌ Erro ao salvar.');
}

function showStatus(el, ok, msg) {
  el.className = 'status ' + (ok ? 'ok' : 'err');
  el.textContent = msg;
  setTimeout(() => { el.className = 'status'; }, 4000);
}

renderTracks();
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleWebSaveClock() {
  if (!server.hasArg("epoch")) {
    server.send(400, "text/plain", "Parametro epoch ausente");
    return;
  }
  unsigned long epoch = server.arg("epoch").toInt();
  if (epoch < 1000000000UL) {
    server.send(400, "text/plain", "Epoch invalido");
    return;
  }
  bootEpoch = (time_t)epoch;
  bootMs    = millis();
  lastEpochSave = millis();
  saveEpochToNVS(bootEpoch);
  server.send(200, "text/plain", "OK");
}

void handleWebSaveTracks() {
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "text/plain", "Body vazio");
    return;
  }
  if (saveAllTrackNames(body)) {
    loadCurrentTrackName();
    if (currentScreen == SCR_PLAYER) updateTrackNameOnDisplay();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(500, "text/plain", "Erro ao salvar");
  }
}

void handleWebGetTracks() {
  if (!LittleFS.exists(TRACKS_FILE)) {
    server.send(200, "application/json", "{}");
    return;
  }
  File f = LittleFS.open(TRACKS_FILE, "r");
  if (!f) { server.send(500, "text/plain", "Erro"); return; }
  String content = f.readString();
  f.close();
  server.send(200, "application/json", content);
}

void handleWebNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}
