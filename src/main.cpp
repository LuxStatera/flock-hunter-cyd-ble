#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

TFT_eSPI tft = TFT_eSPI();

int SW = 320;
int SH = 480;

#define LED_R 4
#define LED_G 16
#define LED_B 17
#define SPEAKER_PIN 26
#define BACKLIGHT_PIN 21
#define SD_CS 5

#define BG    0x0000
#define GRN   0x07E0
#define DGRN  0x0320
#define DDGRN 0x0180
#define RED   0xF800
#define DRED  0x3800
#define GRAY  0x4A69
#define BLU   0x001F
#define DBLU  0x000A

// Flock Safety BLE manufacturer ID
#define FLOCK_MFR_ID 0x09C8

// ═══════════════════════════════════════════════════════════════════════════
// DETECTION STATE
// ═══════════════════════════════════════════════════════════════════════════

struct Det {
    uint8_t mac[6];
    int8_t rssi;
    char name[20];
    unsigned long first, last;
    uint16_t count;
    bool active;
};
#define MAX_DET 50
static Det dets[MAX_DET];
static int nDet = 0;
static volatile int totalScanned = 0;

// SD card state
static bool sdReady = false;
static fs::File csvFile;
static char sessionPath[32];
static unsigned long lastFlush = 0;

enum State { ST_BOOT, ST_SCAN, ST_ALERT, ST_LIST };
static State st = ST_BOOT;
static unsigned long bootT = 0, alertT = 0, lastUI = 0, lastDot = 0;
static int alertIdx = -1, dots = 0;
static State prevSt = ST_BOOT;
static bool needFull = true;
static int prevDots = -1, prevScanned = -1, prevDet = -1, prevActive = -1;
static int prevFlash = -1;
static unsigned long listT = 0;

// BLE scan
BLEScan* pBLEScan = nullptr;
static unsigned long lastScanStart = 0;
#define BLE_SCAN_TIME 1       // seconds per scan cycle
#define BLE_SCAN_INTERVAL 50  // ms between scan restarts

// ═══════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════

void fmtMac(const uint8_t* m, char* b) {
    sprintf(b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}

void setLED(bool r, bool g, bool b) {
    digitalWrite(LED_R, !r); digitalWrite(LED_G, !g); digitalWrite(LED_B, !b);
}

void playTone() {
    ledcAttachPin(SPEAKER_PIN, 0);
    for (int f = 800; f < 2000; f += 100) { ledcWriteTone(0, f); delay(30); }
    ledcWriteTone(0, 0);
    ledcDetachPin(SPEAKER_PIN);
}

const char* rssiLabel(int8_t rssi) {
    if (rssi > -60) return "CLOSE";
    if (rssi > -75) return "NEAR";
    return "FAR";
}

int countActive() {
    int n = 0;
    for (int i = 0; i < nDet; i++)
        if (dets[i].active) n++;
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════
// SD CARD + CSV
// ═══════════════════════════════════════════════════════════════════════════

static int findNextSession() {
    int maxNum = 0;
    fs::File root = SD.open("/flock");
    if (!root) return 1;
    fs::File f = root.openNextFile();
    while (f) {
        const char* name = f.name();
        const char* p = strstr(name, "session_");
        if (p) {
            int n = atoi(p + 8);
            if (n > maxNum) maxNum = n;
        }
        f = root.openNextFile();
    }
    return maxNum + 1;
}

static bool initSD() {
    if (!SD.begin(SD_CS)) {
        Serial.println("[SD] Card init failed");
        return false;
    }
    Serial.printf("[SD] Card ready — %lluMB\n", SD.cardSize() / (1024*1024));

    if (!SD.exists("/flock")) SD.mkdir("/flock");

    int sessNum = findNextSession();
    sprintf(sessionPath, "/flock/session_%03d", sessNum);

    SD.mkdir(sessionPath);
    char csvDir[48];
    sprintf(csvDir, "%s/csv", sessionPath);
    SD.mkdir(csvDir);

    // Open CSV file and write header
    char csvPath[64];
    sprintf(csvPath, "%s/csv/ble_detections.csv", sessionPath);
    csvFile = SD.open(csvPath, FILE_WRITE);
    if (!csvFile) {
        Serial.println("[SD] Failed to create CSV file");
        return false;
    }
    csvFile.println("timestamp_ms,mac,rssi,range,name");
    csvFile.flush();

    Serial.printf("[SD] Session: %s\n", sessionPath);
    return true;
}

static void logDetection(Det& d) {
    if (!sdReady || !csvFile) return;
    char mb[18]; fmtMac(d.mac, mb);
    csvFile.printf("%lu,%s,%d,%s,%s\n",
        millis(), mb, d.rssi, rssiLabel(d.rssi), d.name);

    if (millis() - lastFlush > 5000) {
        csvFile.flush();
        lastFlush = millis();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE DETECTION
// ═══════════════════════════════════════════════════════════════════════════

static void processDevice(BLEAdvertisedDevice& dev) {
    totalScanned++;

    // Check for Flock Safety manufacturer ID
    if (!dev.haveManufacturerData()) return;
    std::string mfrData = dev.getManufacturerData();
    if (mfrData.length() < 2) return;

    // Manufacturer ID is first 2 bytes, little-endian
    uint16_t mfrId = (uint8_t)mfrData[0] | ((uint8_t)mfrData[1] << 8);
    if (mfrId != FLOCK_MFR_ID) return;

    // It's a Flock device!
    uint8_t mac[6];
    memcpy(mac, dev.getAddress().getNative(), 6);

    // Check if already detected
    int f = -1;
    for (int i = 0; i < nDet; i++)
        if (memcmp(dets[i].mac, mac, 6) == 0) { f = i; break; }

    if (f >= 0) {
        dets[f].rssi = dev.getRSSI();
        dets[f].last = millis();
        dets[f].count++;
        dets[f].active = true;
        logDetection(dets[f]);
    } else if (nDet < MAX_DET) {
        Det& d = dets[nDet];
        memcpy(d.mac, mac, 6);
        d.rssi = dev.getRSSI();
        d.first = d.last = millis();
        d.count = 1;
        d.active = true;

        // Capture device name if available
        if (dev.haveName()) {
            strncpy(d.name, dev.getName().c_str(), 19);
            d.name[19] = 0;
        } else {
            strcpy(d.name, "Flock Camera");
        }

        alertIdx = nDet;
        alertT = millis();
        st = ST_ALERT;
        needFull = true;
        playTone();

        logDetection(d);

        char mb[18]; fmtMac(mac, mb);
        Serial.printf("[BLE ALERT] %s RSSI:%d MFR:0x%04X %s\n",
                      mb, d.rssi, mfrId, d.name);
        nDet++;
    }
}

class FlockScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        processDevice(advertisedDevice);
    }
};

// Mark stale devices
static void updateActive() {
    for (int i = 0; i < nDet; i++)
        if (millis() - dets[i].last > 30000) dets[i].active = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// BOOT SCREEN
// ═══════════════════════════════════════════════════════════════════════════
void drawBoot() {
    if (needFull) {
        tft.fillScreen(BG);
        for (int y = 0; y < SH; y += 8)
            tft.drawFastHLine(0, y, SW, DBLU);

        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(BLU, BG);
        tft.setTextFont(4); tft.setTextSize(2);
        tft.drawString("Flock Hunter", SW/2, 80);
        tft.setTextSize(1);

        tft.drawFastHLine(40, 120, SW-80, BLU);
        tft.drawFastHLine(60, 122, SW-120, DBLU);

        tft.setTextFont(2); tft.setTextColor(DBLU, BG);
        tft.drawString("Based on Flock You", SW/2, 145);

        tft.setTextFont(2); tft.setTextColor(DBLU, BG);
        tft.drawString("ESP32-CYD // BLE Edition", SW/2, 185);
        tft.drawString("MFR ID: 0x09C8", SW/2, 205);

        tft.drawRect(40, 240, SW-80, 16, DBLU);

        tft.setTextFont(2); tft.setTextColor(BLU, BG);
        tft.drawString("INITIALIZING BLE", SW/2, 270);
        tft.setTextDatum(TL_DATUM);
        needFull = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SCANNING SCREEN
// ═══════════════════════════════════════════════════════════════════════════
void drawScan() {
    if (needFull) {
        tft.fillScreen(BG);

        // Header bar
        tft.fillRect(0, 0, SW, 30, BLU);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(BG, BLU);
        tft.setTextFont(4);
        tft.drawString("FLOCK HUNTER", SW/2, 16);
        tft.setTextDatum(TL_DATUM);

        // Divider after channels
        tft.drawFastHLine(8, 120, SW-16, DBLU);

        // Labels
        tft.setTextFont(2); tft.setTextColor(DBLU, BG);
        tft.drawString("BLE DEVICES", 15, 130);
        tft.drawString("CAMERAS", SW/2 + 10, 130);

        // Divider
        tft.drawFastHLine(8, 180, SW-16, DBLU);

        // SD status
        tft.setTextFont(2);
        if (sdReady) {
            tft.setTextColor(DBLU, BG);
            tft.drawString("SD: OK   CSV: REC", 15, 210);
        } else {
            tft.setTextColor(DRED, BG);
            tft.drawString("SD: NONE", 15, 210);
        }

        prevDots = -1; prevScanned = -1; prevDet = -1; prevActive = -1;
        needFull = false;
    }

    // "SCANNING..." centered
    if (dots != prevDots) {
        tft.setTextColor(BLU, BG);
        tft.setTextFont(4); tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        int tx = 80;
        int ty = 50;
        tft.drawString("BLE SCAN", tx, ty);
        int dotX = tx + tft.textWidth("BLE SCAN");
        tft.fillRect(dotX, ty, 50, 26, BG);
        char ds[4] = "";
        for (int i = 0; i < dots; i++) strcat(ds, ".");
        tft.drawString(ds, dotX, ty);
        prevDots = dots;
    }

    // BLE mode indicator box (centered)
    static bool modeDrawn = false;
    if (!modeDrawn) {
        int bw = 200, bh = 30;
        int x = (SW - bw) / 2, y = 88;
        tft.fillRoundRect(x, y, bw, bh, 6, BG);
        tft.drawRoundRect(x, y, bw, bh, 6, BLU);
        tft.setTextColor(BLU, BG);
        tft.setTextFont(2);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("MFR ID: 0x09C8", x+bw/2, y+bh/2);
        tft.setTextDatum(TL_DATUM);
        modeDrawn = true;
    }

    // BLE device count
    if (totalScanned != prevScanned) {
        tft.setTextFont(4);
        tft.setTextColor(BLU, BG);
        char b[16]; sprintf(b, "%-8d", totalScanned);
        tft.drawString(b, 15, 150);
        prevScanned = totalScanned;
    }

    // Camera count
    if (nDet != prevDet) {
        tft.setTextFont(4);
        tft.setTextColor(nDet > 0 ? RED : BLU, BG);
        char b[16]; sprintf(b, "%-6d", nDet);
        tft.drawString(b, SW/2 + 10, 150);
        prevDet = nDet;
    }

    // In range count
    int active = countActive();
    if (active != prevActive) {
        tft.setTextFont(2);
        tft.fillRect(15, 190, SW - 30, 16, BG);
        if (active > 0) {
            tft.setTextColor(RED, BG);
            char ab[24]; sprintf(ab, "%d IN RANGE", active);
            tft.drawString(ab, 15, 190);
        } else {
            tft.setTextColor(DBLU, BG);
            tft.drawString("No cameras in range", 15, 190);
        }
        tft.setTextColor(DBLU, BG);
        tft.drawString("PASSIVE BLE", SW - 110, 190);
        prevActive = active;
    }

    // Uptime
    unsigned long sec = millis()/1000;
    char ut[16]; sprintf(ut, "%02lu:%02lu  ", sec/60, sec%60);
    tft.setTextFont(2); tft.setTextColor(DBLU, BG);
    tft.drawString(ut, SW - 70, 210);
}

// ═══════════════════════════════════════════════════════════════════════════
// ALERT SCREEN
// ═══════════════════════════════════════════════════════════════════════════
void drawAlert(int idx) {
    if (idx < 0 || idx >= nDet) return;
    Det& d = dets[idx];
    unsigned long el = millis() - alertT;
    int flashState = (el < 1000) ? (int)((el / 150) % 2) : 2;

    if (flashState == prevFlash && !needFull) return;
    prevFlash = flashState;

    uint16_t bg = (flashState == 1) ? tft.color565(30,0,0) : BG;
    tft.fillScreen(bg);

    // Banner
    uint16_t hbg = (flashState == 0) ? RED : DRED;
    tft.fillRect(4, 4, SW-8, 26, hbg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor((flashState == 0) ? BG : RED, hbg);
    tft.setTextFont(2);
    tft.drawString("FLOCK CAMERA DETECTED", SW/2, 17);
    tft.setTextDatum(TL_DATUM);
    needFull = false;

    int y = 40;

    // Detection type
    tft.setTextFont(2); tft.setTextColor(BLU, bg);
    tft.drawString("BLE DETECTION", 12, y);

    y += 22;
    tft.drawFastHLine(8, y, SW-16, DRED);
    y += 8;

    // MAC Address
    tft.setTextFont(2); tft.setTextColor(DBLU, bg);
    tft.drawString("MAC ADDRESS", 12, y);
    char mb[18]; fmtMac(d.mac, mb);
    tft.setTextColor(BLU, bg);
    tft.drawString(mb, 140, y);

    y += 20;

    // Signal + range
    tft.setTextColor(DBLU, bg);
    tft.drawString("SIGNAL", 12, y);
    tft.setTextColor(BLU, bg);
    char rb[24]; sprintf(rb, "%d dBm  %s", d.rssi, rssiLabel(d.rssi));
    tft.drawString(rb, 140, y);

    y += 20;

    // Manufacturer ID
    tft.setTextColor(DBLU, bg);
    tft.drawString("MFR ID", 12, y);
    tft.setTextColor(BLU, bg);
    tft.drawString("0x09C8 (FLOCK)", 140, y);

    y += 20;

    // Device name
    tft.setTextColor(DBLU, bg);
    tft.drawString("DEVICE NAME", 12, y);
    tft.setTextColor(BLU, bg);
    tft.drawString(d.name, 140, y);

    y += 22;
    tft.drawFastHLine(8, y, SW-16, DRED);
    y += 8;

    // Hits
    tft.setTextColor(DBLU, bg);
    tft.drawString("HITS", 12, y);
    tft.setTextColor(BLU, bg);
    char hb[8]; sprintf(hb, "%d", d.count);
    tft.drawString(hb, 140, y);

    y += 20;

    // Status
    tft.setTextColor(DBLU, bg);
    tft.drawString("STATUS", 12, y);
    tft.setTextColor(d.active ? BLU : RED, bg);
    tft.drawString(d.active ? "LIVE" : "STALE", 140, y);
}

// ═══════════════════════════════════════════════════════════════════════════
// LIST SCREEN
// ═══════════════════════════════════════════════════════════════════════════
void drawList() {
    if (needFull) {
        tft.fillScreen(BG);

        tft.fillRect(0, 0, SW, 30, BLU);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(BG, BLU);
        tft.setTextFont(4);
        char hdr[24]; sprintf(hdr, "FLOCK CAMERAS: %d", nDet);
        tft.drawString(hdr, SW/2, 16);
        tft.setTextDatum(TL_DATUM);

        if (nDet == 0) {
            tft.setTextFont(4); tft.setTextColor(DBLU, BG);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("NO CAMERAS", SW/2, SH/2 - 15);
            tft.drawString("DETECTED", SW/2, SH/2 + 15);
            tft.setTextDatum(TL_DATUM);
            return;
        }

        int maxVis = 4;
        int show = min(nDet, maxVis);

        for (int r = 0; r < show; r++) {
            Det& d = dets[nDet - 1 - r];
            int y = 45 + r * 50;
            uint16_t fg = d.active ? BLU : GRAY;

            tft.fillCircle(10, y+8, 4, d.active ? BLU : RED);

            char mb[18]; fmtMac(d.mac, mb);
            tft.setTextFont(2); tft.setTextColor(fg, BG);
            tft.drawString(mb, 22, y);

            tft.setTextFont(2); tft.setTextColor(DBLU, BG);
            char info[48];
            sprintf(info, "%ddBm %s  BLE  %s", d.rssi, rssiLabel(d.rssi), d.name);
            tft.drawString(info, 22, y + 22);

            tft.drawFastHLine(8, y + 42, SW-16, DBLU);
        }
        needFull = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n[FLOCK HUNTER BLE] Booting...");

    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
    setLED(false, false, true);  // Blue for boot

    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, LOW);

    tft.init();
    tft.setRotation(2);
    tft.invertDisplay(true);
    tft.fillScreen(BG);
    digitalWrite(BACKLIGHT_PIN, HIGH);

    SW = tft.width();
    SH = tft.height();
    Serial.printf("[DISPLAY] w=%d h=%d (ILI9488)\n", SW, SH);

    // Draw boot screen
    bootT = millis();
    st = ST_BOOT;
    needFull = true;
    drawBoot();

    // Init SD card
    sdReady = initSD();

    // Init BLE
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new FlockScanCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    // Hold boot screen
    while (millis() - bootT < 3000) {
        delay(30);
    }

    st = ST_SCAN;
    needFull = true;
    lastScanStart = millis();
    pBLEScan->start(1, false);

    Serial.println("[FLOCK HUNTER BLE] Scanning for MFR ID 0x09C8");
}

// ═══════════════════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // Restart BLE scan periodically — short bursts for responsive UI
    if (now - lastScanStart > 1500) {
        pBLEScan->stop();
        pBLEScan->clearResults();
        pBLEScan->start(1, false);
        lastScanStart = now;
    }

    updateActive();

    if (now - lastDot >= 300) { lastDot = now; dots = (dots+1) % 4; }
    if (st != prevSt) { needFull = true; prevSt = st; }

    switch (st) {
        case ST_BOOT:
            st = ST_SCAN; needFull = true;
            break;

        case ST_ALERT:
            analogWrite(LED_G, 255);    // stop PWM breathing
            setLED(true, false, false); // red
            if (now - alertT < 5000) {
                if (now - lastUI >= 200) { lastUI = now; drawAlert(alertIdx); }
            } else {
                st = nDet > 0 ? ST_LIST : ST_SCAN;
                listT = now;
                setLED(false,true,false); needFull = true;
            }
            break;

        case ST_SCAN:
            if (now - lastUI >= 100) { lastUI = now; drawScan(); }
            break;

        case ST_LIST:
            if (now - lastUI >= 500) { lastUI = now; drawList(); }
            if (now - listT > 5000) {
                st = ST_SCAN; needFull = true;
            }
            break;
    }

    // Serial reporting
    static int lastRep = 0;
    if (nDet > lastRep) {
        for (int i = lastRep; i < nDet; i++) {
            char mb[18]; fmtMac(dets[i].mac, mb);
            Serial.printf("[BLE ALERT] %s RSSI:%d %s\n",
                          mb, dets[i].rssi, dets[i].name);
        }
        lastRep = nDet;
    }

    // Green breathing during scan
    if (st == ST_SCAN || st == ST_LIST) {
        digitalWrite(LED_R, HIGH);
        digitalWrite(LED_B, HIGH);
        int b = (sin(now / 500.0) + 1.0) * 127;
        analogWrite(LED_G, 255-b);
    }

    delay(1);
}
