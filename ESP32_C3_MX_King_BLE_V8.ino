#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

// ================= PENGATURAN BLUETOOTH BLE =================
#define BLE_DEVICE_NAME   "MX_King_LED"
#define SERVICE_UUID      "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_UUID_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_UUID_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// ================= PENGATURAN PIN & JUMLAH LED (25 + 25) =================
#define PIN_LED_KIRI      4
#define PIN_LED_KANAN     5
#define PIN_LED_BELAKANG  6
#define PIN_SEIN_KIRI     0
#define PIN_SEIN_KANAN    1

#define NUM_LEDS_DEPAN    25
#define TOTAL_DEPAN       50
#define NUM_LEDS_BELAKANG 36
#define MIRROR_KIRI       true

Adafruit_NeoPixel stripKiri(NUM_LEDS_DEPAN, PIN_LED_KIRI, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripKanan(NUM_LEDS_DEPAN, PIN_LED_KANAN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripBelakang(NUM_LEDS_BELAKANG, PIN_LED_BELAKANG, NEO_GRB + NEO_KHZ800);
Preferences pref;

BLEServer* pServer = nullptr;
BLECharacteristic* pTxChar = nullptr;
volatile bool deviceConnected = false;
volatile bool needRestartAdv = false;
unsigned long disconnectTime = 0;

#define CMD_Q_SIZE 8
#define CMD_MAX_LEN 64
char cmdQueue[CMD_Q_SIZE][CMD_MAX_LEN];
volatile int qHead = 0;
volatile int qTail = 0;

int currentMode = 1;
int globalBrightness = 255;
int speedDelay = 35;
int runLength = 3;
int red = 0, green = 0, blue = 255;
bool needStaticUpdate = true;

unsigned long lastUpdate = 0;
int tumpukPos = 0, tumpukEnd = NUM_LEDS_DEPAN;
int tumpukPosBlk = 0, tumpukEndBlk = NUM_LEDS_BELAKANG;
bool isPaused = false;
unsigned long pauseStart = 0;
int runPos = 0, runPosBlk = 0;
float wipeF = 0.0f, wipeFBlk = 0.0f;
int ppPos = 0, ppDir = 1, ppPosBlk = 0, ppDirBlk = 1;
int ppTumpukL = 0, ppTumpukR = TOTAL_DEPAN, ppTPos = 0, ppTDir = 1;
int ppTumpukLBlk = 0, ppTumpukRBlk = NUM_LEDS_BELAKANG, ppTPosBlk = 0, ppTDirBlk = 1;
bool ppTPaused = false;
unsigned long ppTPauseStart = 0;

unsigned long lastSeinKiriTime = 0;
unsigned long lastSeinKananTime = 0;
const unsigned long SEIN_HOLD_MS = 650;
unsigned long lastSeinAnim = 0;
int seinStep = 0;
bool prevAdaSein = false;
int appSeinTest = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    deviceConnected = true;
    needRestartAdv = false;
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    needRestartAdv = true;
    disconnectTime = millis();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    uint8_t* data = pChar->getData();
    size_t len = pChar->getLength();
    if (!data || len == 0) return;
    int nextHead = (qHead + 1) % CMD_Q_SIZE;
    if (nextHead == qTail) return;
    size_t copyLen = (len < CMD_MAX_LEN - 1) ? len : (CMD_MAX_LEN - 1);
    memcpy(cmdQueue[qHead], data, copyLen);
    cmdQueue[qHead][copyLen] = '\0';
    qHead = nextHead;
  }
};

void setLeftFromCenter(int i, uint32_t col) {
  if (i < 0 || i >= NUM_LEDS_DEPAN) return;
  int pIdx = MIRROR_KIRI ? i : (NUM_LEDS_DEPAN - 1 - i);
  stripKiri.setPixelColor(pIdx, col);
}
void setFrontSym(int i, uint32_t col) {
  if (i < 0 || i >= NUM_LEDS_DEPAN) return;
  setLeftFromCenter(i, col);
  stripKanan.setPixelColor(i, col);
}
void setFrontVirtual(int idx, uint32_t col) {
  if (idx < 0 || idx >= TOTAL_DEPAN) return;
  if (idx < NUM_LEDS_DEPAN) setLeftFromCenter(NUM_LEDS_DEPAN - 1 - idx, col);
  else stripKanan.setPixelColor(idx - NUM_LEDS_DEPAN, col);
}
void clearAllStrips() { stripKiri.clear(); stripKanan.clear(); stripBelakang.clear(); }

void resetModeState(int m) {
  lastUpdate = millis();
  if (m == 1) { tumpukPos = 0; tumpukEnd = NUM_LEDS_DEPAN; tumpukPosBlk = 0; tumpukEndBlk = NUM_LEDS_BELAKANG; isPaused = false; }
  if (m == 2) { runPos = 0; runPosBlk = 0; }
  if (m == 3) { needStaticUpdate = true; }
  if (m == 4) { wipeF = 0.0f; wipeFBlk = 0.0f; clearAllStrips(); }
  if (m == 5) { ppPos = 0; ppDir = 1; ppPosBlk = 0; ppDirBlk = 1; clearAllStrips(); }
  if (m == 6) {
    ppTumpukL = 0; ppTumpukR = TOTAL_DEPAN; ppTPos = 0; ppTDir = 1;
    ppTumpukLBlk = 0; ppTumpukRBlk = NUM_LEDS_BELAKANG; ppTPosBlk = 0; ppTDirBlk = 1;
    ppTPaused = false; clearAllStrips();
  }
}

void sendConfigToApp() {
  if (!deviceConnected || !pTxChar) return;
  pref.begin("ledData", true);
  char buf[96];
  snprintf(buf, sizeof(buf), "CFG:%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
    currentMode, red, green, blue, globalBrightness, speedDelay, runLength,
    pref.getInt("b_m", 1), pref.getInt("b_r", 0), pref.getInt("b_g", 0),
    pref.getInt("b_b", 255), pref.getInt("b_brg", 255),
    pref.getInt("b_spd", 35), pref.getInt("b_len", 3)
  );
  pref.end();
  pTxChar->setValue((uint8_t*)buf, strlen(buf));
  pTxChar->notify();
}

void processCommand(const String& cmd) {
  if (cmd == "GET") { sendConfigToApp(); }
  else if (cmd.startsWith("M:")) {
    int m = cmd.substring(2).toInt();
    if (m >= 1 && m <= 6) { currentMode = m; resetModeState(currentMode); }
  }
  else if (cmd.startsWith("C:")) {
    int nr, ng, nb;
    if (sscanf(cmd.substring(2).c_str(), "%d,%d,%d", &nr, &ng, &nb) == 3) {
      red = constrain(nr, 0, 255); green = constrain(ng, 0, 255); blue = constrain(nb, 0, 255);
      needStaticUpdate = true;
    }
  }
  else if (cmd.startsWith("B:")) {
    globalBrightness = constrain(cmd.substring(2).toInt(), 0, 255);
    stripKiri.setBrightness(globalBrightness); stripKanan.setBrightness(globalBrightness); stripBelakang.setBrightness(globalBrightness);
    needStaticUpdate = true;
  }
  else if (cmd.startsWith("S:")) { speedDelay = constrain(cmd.substring(2).toInt(), 5, 100); }
  else if (cmd.startsWith("L:")) { runLength = constrain(cmd.substring(2).toInt(), 1, 10); }
  else if (cmd.startsWith("BOOT:")) {
    int bm, br, bg, bb, bbrg, bspd, blen;
    if (sscanf(cmd.substring(5).c_str(), "%d,%d,%d,%d,%d,%d,%d", &bm, &br, &bg, &bb, &bbrg, &bspd, &blen) == 7) {
      pref.begin("ledData", false);
      pref.putInt("b_m", constrain(bm, 1, 6)); pref.putInt("b_r", constrain(br, 0, 255));
      pref.putInt("b_g", constrain(bg, 0, 255)); pref.putInt("b_b", constrain(bb, 0, 255));
      pref.putInt("b_brg", constrain(bbrg, 0, 255)); pref.putInt("b_spd", constrain(bspd, 5, 100));
      pref.putInt("b_len", constrain(blen, 1, 10));
      pref.end();
      if (deviceConnected && pTxChar) {
        const char* okMsg = "BOOT_OK";
        pTxChar->setValue((uint8_t*)okMsg, strlen(okMsg));
        pTxChar->notify();
      }
    }
  }
  else if (cmd.startsWith("SEIN:")) {
    String s = cmd.substring(5);
    if (s == "L" || s == "1") appSeinTest = 1;
    else if (s == "R" || s == "2") appSeinTest = 2;
    else if (s == "H" || s == "3") appSeinTest = 3;
    else appSeinTest = 0;
  }
}

void setup() {
  pinMode(PIN_SEIN_KIRI, INPUT_PULLUP);
  pinMode(PIN_SEIN_KANAN, INPUT_PULLUP);

  pref.begin("ledData", true);
  currentMode = pref.getInt("b_m", 1); globalBrightness = pref.getInt("b_brg", 255);
  speedDelay = pref.getInt("b_spd", 35); runLength = pref.getInt("b_len", 3);
  red = pref.getInt("b_r", 0); green = pref.getInt("b_g", 0); blue = pref.getInt("b_b", 255);
  pref.end();

  stripKiri.begin(); stripKiri.setBrightness(globalBrightness); stripKiri.clear(); stripKiri.show();
  stripKanan.begin(); stripKanan.setBrightness(globalBrightness); stripKanan.clear(); stripKanan.show();
  stripBelakang.begin(); stripBelakang.setBrightness(globalBrightness); stripBelakang.clear(); stripBelakang.show();

  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pTxChar = pService->createCharacteristic(CHAR_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pTxChar->addDescriptor(new BLE2902());
  BLECharacteristic *pRxChar = pService->createCharacteristic(CHAR_UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
}

void loop() {
  unsigned long now = millis();
  if (needRestartAdv && (now - disconnectTime >= 350)) { needRestartAdv = false; BLEDevice::startAdvertising(); }

  while (qTail != qHead) {
    String cmd = String(cmdQueue[qTail]);
    qTail = (qTail + 1) % CMD_Q_SIZE;
    cmd.trim();
    if (cmd.length() > 0) processCommand(cmd);
  }

  bool frameUpdated = false;
  if (digitalRead(PIN_SEIN_KIRI) == LOW)  lastSeinKiriTime = now;
  if (digitalRead(PIN_SEIN_KANAN) == LOW) lastSeinKananTime = now;

  bool seinKiriAktif  = (lastSeinKiriTime != 0 && (now - lastSeinKiriTime < SEIN_HOLD_MS)) || (appSeinTest == 1) || (appSeinTest == 3);
  bool seinKananAktif = (lastSeinKananTime != 0 && (now - lastSeinKananTime < SEIN_HOLD_MS)) || (appSeinTest == 2) || (appSeinTest == 3);
  bool hazardAktif    = (seinKiriAktif && seinKananAktif);
  bool adaSein        = (seinKiriAktif || seinKananAktif);

  if (adaSein && !prevAdaSein) { seinStep = 0; lastSeinAnim = now; frameUpdated = true; }
  else if (!adaSein && prevAdaSein) { seinStep = 0; needStaticUpdate = true; }
  prevAdaSein = adaSein;

  int safeDelay = (speedDelay < 5) ? 5 : speedDelay;
  int safeLen   = (runLength < 1) ? 1 : runLength;
  uint32_t col  = stripKiri.Color(red, green, blue);

  if (currentMode == 1) {
    if (isPaused) { if (now - pauseStart > 400) isPaused = false; }
    else if (now - lastUpdate >= (unsigned long)safeDelay) {
      lastUpdate = now; frameUpdated = true; clearAllStrips();
      for (int i = tumpukEnd; i < NUM_LEDS_DEPAN; i++) setFrontSym(i, col);
      for (int i = 0; i < safeLen; i++) { int px = tumpukPos - i; if (px >= 0 && px < tumpukEnd) setFrontSym(px, col); }
      tumpukPos++;
      if (tumpukPos >= tumpukEnd) { tumpukEnd -= safeLen; tumpukPos = 0; }
      if (tumpukEnd <= 0) { tumpukEnd = NUM_LEDS_DEPAN; tumpukPos = 0; isPaused = true; pauseStart = now; }

      for (int i = tumpukEndBlk; i < NUM_LEDS_BELAKANG; i++) stripBelakang.setPixelColor(i, col);
      for (int i = 0; i < safeLen; i++) { int px = tumpukPosBlk - i; if (px >= 0 && px < tumpukEndBlk) stripBelakang.setPixelColor(px, col); }
      tumpukPosBlk++;
      if (tumpukPosBlk >= tumpukEndBlk) { tumpukEndBlk -= safeLen; tumpukPosBlk = 0; }
      if (tumpukEndBlk <= 0) { tumpukEndBlk = NUM_LEDS_BELAKANG; tumpukPosBlk = 0; }
    }
  } else if (currentMode == 2) {
    if (now - lastUpdate >= (unsigned long)safeDelay) {
      lastUpdate = now; frameUpdated = true;
      uint32_t dimCol = stripKiri.Color(red >> 3, green >> 3, blue >> 3);
      for (int i = 0; i < NUM_LEDS_DEPAN; i++) setFrontSym(i, dimCol);
      for (int i = 0; i < safeLen; i++) setFrontSym((runPos - i + NUM_LEDS_DEPAN) % NUM_LEDS_DEPAN, col);
      runPos = (runPos + 1) % NUM_LEDS_DEPAN;
      for (int i = 0; i < NUM_LEDS_BELAKANG; i++) stripBelakang.setPixelColor(i, dimCol);
      for (int i = 0; i < safeLen; i++) stripBelakang.setPixelColor((runPosBlk - i + NUM_LEDS_BELAKANG) % NUM_LEDS_BELAKANG, col);
      runPosBlk = (runPosBlk + 1) % NUM_LEDS_BELAKANG;
    }
  } else if (currentMode == 3) {
    if (needStaticUpdate || adaSein) {
      needStaticUpdate = false; frameUpdated = true;
      for (int i = 0; i < NUM_LEDS_DEPAN; i++) setFrontSym(i, col);
      for (int i = 0; i < NUM_LEDS_BELAKANG; i++) stripBelakang.setPixelColor(i, col);
    }
  } else if (currentMode == 4) {
    if (now - lastUpdate >= 16) {
      unsigned long dt = now - lastUpdate; if (dt > 100) dt = 100;
      lastUpdate = now; frameUpdated = true; clearAllStrips();
      float step = (float)dt / (float)safeDelay;

      wipeF += step; if (wipeF >= (float)(NUM_LEDS_DEPAN + safeLen)) wipeF = 0.0f;
      float tailF = wipeF - (float)safeLen;
      for (int i = 0; i < NUM_LEDS_DEPAN; i++) {
        float ov = min(wipeF, (float)(i + 1)) - max(tailF, (float)i);
        if (ov > 0.0f) { if (ov > 1.0f) ov = 1.0f; float g = ov * ov; setFrontSym(i, stripKiri.Color((uint8_t)(red*g), (uint8_t)(green*g), (uint8_t)(blue*g))); }
      }
      wipeFBlk += step; if (wipeFBlk >= (float)(NUM_LEDS_BELAKANG + safeLen)) wipeFBlk = 0.0f;
      float tailBlk = wipeFBlk - (float)safeLen;
      for (int i = 0; i < NUM_LEDS_BELAKANG; i++) {
        float ov = min(wipeFBlk, (float)(i + 1)) - max(tailBlk, (float)i);
        if (ov > 0.0f) { if (ov > 1.0f) ov = 1.0f; float g = ov * ov; stripBelakang.setPixelColor(i, stripBelakang.Color((uint8_t)(red*g), (uint8_t)(green*g), (uint8_t)(blue*g))); }
      }
    }
  } else if (currentMode == 5) {
    if (now - lastUpdate >= (unsigned long)safeDelay) {
      lastUpdate = now; frameUpdated = true; clearAllStrips();
      for (int i = 0; i < safeLen; i++) { int px = ppPos + i; if (px >= 0 && px < TOTAL_DEPAN) setFrontVirtual(px, col); }
      ppPos += ppDir;
      if (ppPos + safeLen >= TOTAL_DEPAN) { ppPos = TOTAL_DEPAN - safeLen; ppDir = -1; } else if (ppPos <= 0) { ppPos = 0; ppDir = 1; }
      for (int i = 0; i < safeLen; i++) { int px = ppPosBlk + i; if (px >= 0 && px < NUM_LEDS_BELAKANG) stripBelakang.setPixelColor(px, col); }
      ppPosBlk += ppDirBlk;
      if (ppPosBlk + safeLen >= NUM_LEDS_BELAKANG) { ppPosBlk = NUM_LEDS_BELAKANG - safeLen; ppDirBlk = -1; } else if (ppPosBlk <= 0) { ppPosBlk = 0; ppDirBlk = 1; }
    }
  } else if (currentMode == 6) {
    if (ppTPaused) { if (now - ppTPauseStart > 400) { ppTPaused = false; resetModeState(6); } }
    else if (now - lastUpdate >= (unsigned long)safeDelay) {
      lastUpdate = now; frameUpdated = true; clearAllStrips();
      for (int i = 0; i < ppTumpukL; i++) setFrontVirtual(i, col);
      for (int i = ppTumpukR; i < TOTAL_DEPAN; i++) setFrontVirtual(i, col);
      for (int i = 0; i < safeLen; i++) { int px = ppTPos + i; if (px >= ppTumpukL && px < ppTumpukR) setFrontVirtual(px, col); }
      if (ppTDir == 1) {
        ppTPos++;
        if (ppTPos + safeLen >= ppTumpukR) {
          ppTumpukR -= safeLen;
          if (ppTumpukR <= ppTumpukL) { for (int i = 0; i < TOTAL_DEPAN; i++) setFrontVirtual(i, col); ppTPaused = true; ppTPauseStart = now; }
          else { ppTDir = -1; ppTPos = max(ppTumpukL, ppTumpukR - safeLen); }
        }
      } else {
        ppTPos--;
        if (ppTPos <= ppTumpukL) {
          ppTumpukL += safeLen;
          if (ppTumpukL >= ppTumpukR) { for (int i = 0; i < TOTAL_DEPAN; i++) setFrontVirtual(i, col); ppTPaused = true; ppTPauseStart = now; }
          else { ppTDir = 1; ppTPos = ppTumpukL; }
        }
      }
      for (int i = 0; i < ppTumpukLBlk; i++) stripBelakang.setPixelColor(i, col);
      for (int i = ppTumpukRBlk; i < NUM_LEDS_BELAKANG; i++) stripBelakang.setPixelColor(i, col);
      for (int i = 0; i < safeLen; i++) { int px = ppTPosBlk + i; if (px >= ppTumpukLBlk && px < ppTumpukRBlk) stripBelakang.setPixelColor(px, col); }
      if (ppTDirBlk == 1) {
        ppTPosBlk++;
        if (ppTPosBlk + safeLen >= ppTumpukRBlk) {
          ppTumpukRBlk -= safeLen;
          if (ppTumpukRBlk <= ppTumpukLBlk) { ppTumpukLBlk = 0; ppTumpukRBlk = NUM_LEDS_BELAKANG; ppTPosBlk = 0; ppTDirBlk = 1; }
          else { ppTDirBlk = -1; ppTPosBlk = max(ppTumpukLBlk, ppTumpukRBlk - safeLen); }
        }
      } else {
        ppTPosBlk--;
        if (ppTPosBlk <= ppTumpukLBlk) {
          ppTumpukLBlk += safeLen;
          if (ppTumpukLBlk >= ppTumpukRBlk) { ppTumpukLBlk = 0; ppTumpukRBlk = NUM_LEDS_BELAKANG; ppTPosBlk = 0; ppTDirBlk = 1; }
          else { ppTDirBlk = 1; ppTPosBlk = ppTumpukLBlk; }
        }
      }
    }
  }

  if (adaSein) {
    if (now - lastSeinAnim >= 20) {
      lastSeinAnim = now; frameUpdated = true;
      seinStep = (seinStep + 1) % NUM_LEDS_DEPAN;
    }
    if (frameUpdated) {
      uint32_t kuningSein = stripKiri.Color(255, 110, 0);
      uint32_t putihDiam  = stripKiri.Color(255, 255, 255);
      if (hazardAktif) {
        stripKiri.clear(); stripKanan.clear();
        for (int i = 0; i <= seinStep; i++) { setLeftFromCenter(i, kuningSein); stripKanan.setPixelColor(i, kuningSein); }
      } else if (seinKiriAktif) {
        stripKiri.clear();
        for (int i = 0; i <= seinStep; i++) setLeftFromCenter(i, kuningSein);
        for (int i = 0; i < NUM_LEDS_DEPAN; i++) stripKanan.setPixelColor(i, putihDiam);
      } else if (seinKananAktif) {
        stripKanan.clear();
        for (int i = 0; i <= seinStep; i++) stripKanan.setPixelColor(i, kuningSein);
        for (int i = 0; i < NUM_LEDS_DEPAN; i++) setLeftFromCenter(i, putihDiam);
      }
    }
  }

  if (frameUpdated) { stripKiri.show(); stripKanan.show(); stripBelakang.show(); }
}
