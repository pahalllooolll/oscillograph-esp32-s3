#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h> 
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite menuSpr = TFT_eSprite(&tft); 
Preferences prefs;
WebServer server(80);
Adafruit_NeoPixel led(1, 48, NEO_GRB + NEO_KHZ800);

// --- НАСТРОЙКИ КНОПОК И ПЕРИФЕРИИ ---
#define BTN_UP        3
#define BTN_DOWN      46
#define BTN_LEFT      10
#define BTN_RIGHT     11
#define BTN_OK        12

#define TEST_WAVE_PIN 13  
#define TFT_BL_PIN    18     
#define BUZZER_PIN    14     
#define I2C_SDA_PIN   21 
#define I2C_SCL_PIN   22

bool lastStateUP = HIGH, lastStateDOWN = HIGH, lastStateLEFT = HIGH, lastStateRIGHT = HIGH, lastStateOK = HIGH;

// --- НАСТРОЙКА ПИНОВ И ЦВЕТОВ ---
const int adcPins[] = {4, 5, 9};
const int NUM_ADC_PINS = 3;
bool pinEnabled[NUM_ADC_PINS] = {true, true, true}; 
int pinColorIndices[NUM_ADC_PINS] = {5, 2, 6};
const uint16_t avaliableColors[] = {TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA};
const char* colorNames[] = {"WHITE", "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA"};

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ГРАФИКА ---
volatile float voltage = 0.0;
int xPos = 0, prevX = 0;
int prevYPins[NUM_ADC_PINS] = {120, 120, 120};

const int WAVE_BUFFER_SIZE = 60;
int waveBuffer[WAVE_BUFFER_SIZE];
int refBuffer[WAVE_BUFFER_SIZE];
int waveBufferIdx = 0;
volatile int currentMinAdc = 4095, currentMaxAdc = 0;
volatile int lastMinAdc = 0, lastMaxAdc = 4095;
volatile int crossings = 0;
unsigned long sweepStartTime = 0;
volatile float freqHz = 0.0, vMax = 0.0, vMin = 0.0, vPP = 0.0, vAvg = 0.0;
bool isFrozen = false, wasAboveMidpoint = false;
bool showReference = false;

// Мьютекс для защиты буфера между ядрами
SemaphoreHandle_t dataMutex;

// --- ПАРАМЕТРЫ МЕНЮ ---
int graphSpeed = 10, smoothLevel = 2, zoomIndex = 0;
bool useGrid = true;
int ledMode = 2, ledColorIndex = 0, testFreqIndex = 2;   
bool wifiEnabled = true;
int brightnessIndex = 0;
volatile bool generatorChanged = false;
int triggerMode = 0, couplingMode = 0, vDivIndex = 1, themeColorIdx = 0;
bool buzzerEnabled = false;
int screenRotation = 1;
bool waterfallEn = false;
int pwmDuty = 50;
const int zoomLevels[] = {1, 2, 4, 8};
const char* smoothNames[] = {"DISABLED", "LIGHT", "MEDIUM", "STRICT"};
const char* ledModes[] = {"DISABLED", "STATIC", "RAINBOW", "SWEEP"};
const char* ledColorNames[] = {"WHITE", "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA"};
const int brightnessLevels[] = {255, 128, 50};
const char* brightnessNames[] = {"100%", "50%", "20%"};
const int testFreqs[] = {10, 50, 100, 500, 1000, 5000, 10000, 100000};
const char* freqNames[] = {"10 Hz", "50 Hz", "100 Hz", "500 Hz", "1 kHz", "5 kHz", "10 kHz", "100 kHz"};
const int NUM_FREQS = 8;
const uint8_t ledRGBValues[][3] = {{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
const uint16_t themeColors[] = {TFT_CYAN, TFT_YELLOW, TFT_RED, TFT_GREEN, TFT_WHITE};
const char* themeNames[] = {"CYAN Neon", "GOLD Cyber", "RED Plasma", "GREEN Matrix", "DOT-MATRIX"};
const char* rotationNames[] = {"0 DEG", "90 DEG", "180 DEG", "270 DEG"};

uint8_t rainbowHue = 0;
unsigned long lastLedUpdate = 0;
int sweepDirection = 1, sweepVal = 0;

// --- МЕНЮ ---
int currentMode = 0;
int menuCursor = 0, menuScrollOffset = 0;
const int MAX_VISIBLE_ITEMS = 6; 
const int MENU_ITEMS_COUNT = 21;
float currentScrollY = 0.0, currentVisualY = 2.0;
float channelsScrollY = 0.0, channelsVisualY = 2.0;
int pulseState = 0, pulseDir = 1, pinsSubCursor = 0, wifiSubCursor = 0;

// Таймер Хинкали
int khinkaliTimerSec = 420;
bool khinkaliRunning = false; unsigned long lastKhinkaliSecTick = 0; int steamAnimFrame = 0;

// ПРОТОТИПЫ
void loadSettings(); void saveSettings(); void handleButtons();
void drawOscilloscope(); void drawGrid(); void updateLED(); void handleLEDAnimations(); uint32_t ledWheel(uint8_t wheelPos);
void initMenuScreen(); void updateMenuDisplay(); void updatePinsMenuDisplay(); void updateWiFiMenuDisplay();
void updateGenerator(); void toggleWiFi(); void updateBrightness(); void drawKhinkaliScreen();
void runI2CRadar(); void runPWMTool(); void drawLissajous();

void beep(int freq, int duration) { 
  if (buzzerEnabled) tone(BUZZER_PIN, freq, duration);
}

// --- ВЕБ ИНТЕРФЕЙС ---
const char PAGE_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Quantum Scope UI</title>
<style>
:root { --bg: #05050a; --card: #0d0d1f; --accent: #00ffcc; --magenta: #ff007f; --text: #e0e0ff; }
body { background: var(--bg); color: var(--text); font-family: sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
h2 { color: var(--accent); } #waveCanvas { background: #020205; border: 2px solid #1a1a3a; width: 100%; max-width: 550px; }
.grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 15px; width: 100%; max-width: 550px; margin: 20px 0; }
.card { background: var(--card); padding: 15px; text-align: center; border: 1px solid #1a1a3a; }
</style></head><body><h2>⚡ QUANTUM SCOPE OS ⚡</h2>
<canvas id="waveCanvas" width="550" height="240"></canvas>
<div class='grid'><div class='card'><div>Amplitude</div><div id='v' style='color:var(--accent)'>-- V</div></div>
<div class='card'><div>Frequency</div><div id='freq' style='color:var(--magenta)'>-- Hz</div></div></div>
<script>
let canvas = document.getElementById('waveCanvas'); let ctx = canvas.getContext('2d');
setInterval(async () => {
try { let r = await fetch('/data'); let j = await r.json();
document.getElementById('v').innerText = j.v.toFixed(2) + ' V';
document.getElementById('freq').innerText = j.freq + ' Hz';
ctx.clearRect(0, 0, canvas.width, canvas.height); ctx.strokeStyle = '#00ffcc'; ctx.beginPath();
for(let i=0; i<j.wave.length; i++) {
let x = (i / (j.wave.length-1)) * canvas.width; let y = canvas.height - ((j.wave[i] / 4095) * canvas.height);
if(i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
} ctx.stroke(); }catch(e){} }, 150);
</script></body></html>)rawliteral";

void handleRoot() { server.send_P(200, "text/html", PAGE_MAIN); }

void handleData() {
  String json;
  json.reserve(1024); 
  
  if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
    json = "{\"v\":" + String(voltage) + ",\"freq\":" + String(freqHz) + ",\"wave\":[";
    for (int i = 0; i < WAVE_BUFFER_SIZE; i++) { 
      int idx = (waveBufferIdx + i) % WAVE_BUFFER_SIZE;
      json += String(waveBuffer[idx]); 
      if (i < WAVE_BUFFER_SIZE - 1) json += ","; 
    }
    json += "]}"; 
    xSemaphoreGive(dataMutex);
  } else {
    json = "{}"; 
  }
  server.send(200, "application/json", json);
}

void core0Task(void * pvParameters) {
  if (wifiEnabled) { WiFi.softAP("ESP32-Scope", "12345678"); server.on("/", handleRoot); server.on("/data", handleData); server.begin(); }
  for (;;) { if (wifiEnabled) server.handleClient(); vTaskDelay(5 / portTICK_PERIOD_MS); }
}

void setup() {
  Serial.begin(115200);
  delay(500); 
  
  dataMutex = xSemaphoreCreateMutex();
  
  pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP); pinMode(BTN_RIGHT, INPUT_PULLUP); pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  #if ESP_IDF_VERSION_MAJOR >= 5
    ledcAttach(TFT_BL_PIN, 5000, 8);
  #else
    ledcSetup(1, 5000, 8); 
    ledcAttachPin(TFT_BL_PIN, 1);
  #endif

  loadSettings();
  led.begin(); led.setBrightness(50); updateBrightness();
  
  xTaskCreatePinnedToCore(core0Task, "Core0Task", 8192, NULL, 1, NULL, 0);
  updateGenerator(); 
  
  tft.init(); 
  tft.setRotation(screenRotation); 
  tft.fillScreen(TFT_BLACK);
  menuSpr.createSprite(tft.width(), 160);
  sweepStartTime = millis(); updateLED(); 
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

void loop() {
  handleButtons(); 
  handleLEDAnimations();
  if (generatorChanged) { generatorChanged = false; updateGenerator(); }
  
  if (currentMode == 0) { 
    if (!isFrozen) drawOscilloscope();
    if (graphSpeed == 0) delayMicroseconds(10); 
  }
  else if (currentMode == 1) updateMenuDisplay();
  else if (currentMode == 4) updatePinsMenuDisplay();
  else if (currentMode == 5) drawKhinkaliScreen();
  else if (currentMode == 6) runI2CRadar();
  else if (currentMode == 7) runPWMTool();
  else if (currentMode == 8) { if (!isFrozen) drawLissajous(); }
  delay(2);
}

// --- ОТРИСОВКА ИНТЕРФЕЙСОВ ---
void initMenuScreen() {
  tft.fillScreen(TFT_BLACK); tft.setTextColor(themeColors[themeColorIdx], TFT_BLACK);
  tft.drawString("⚡ SYSTEM CONFIG ⚡", 40, 10, 2);
  tft.drawFastHLine(0, 32, tft.width(), tft.color565(40, 40, 80));
  tft.drawFastHLine(0, tft.height()-10, tft.width(), tft.color565(40, 40, 80));
}

void updateMenuDisplay() {
  if (menuCursor < menuScrollOffset) menuScrollOffset = menuCursor;
  else if (menuCursor >= menuScrollOffset + MAX_VISIBLE_ITEMS) menuScrollOffset = menuCursor - MAX_VISIBLE_ITEMS + 1;
  float targetScrollY = menuScrollOffset * 26.0;
  currentScrollY += (targetScrollY - currentScrollY) * 0.20; 
  float targetVisualY = (menuCursor * 26.0) - currentScrollY + 2.0;
  currentVisualY += (targetVisualY - currentVisualY) * 0.30; 
  pulseState += pulseDir * 4;
  if (pulseState >= 50 || pulseState <= 0) pulseDir = -pulseDir;

  menuSpr.fillSprite(TFT_BLACK);
  uint16_t sliderColor = menuSpr.color565(40 + pulseState, 40 + pulseState, 90 + pulseState / 2);
  menuSpr.fillRect(10, (int)currentVisualY, menuSpr.width() - 20, 24, sliderColor);

  for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
    float yTextPos = 5.0 + (i * 26.0) - currentScrollY;
    if (yTextPos < -20 || yTextPos > 170) continue; 
    if (i == menuCursor) menuSpr.setTextColor(TFT_GREEN, sliderColor); else menuSpr.setTextColor(TFT_WHITE, TFT_BLACK);
    String itemText = "";
    switch(i) {
      case 0:  itemText = "-> CHANNELS CONFIG"; break;
      case 1:  itemText = "ZOOM: " + String(zoomLevels[zoomIndex]) + "x"; break;
      case 2:  itemText = "FILT: " + String(smoothNames[smoothLevel]); break;
      case 3:  itemText = "SPEED: " + String(graphSpeed) + "ms"; break;
      case 4:  itemText = "GRID: " + String(useGrid ? "ON" : "OFF"); break;
      case 5:  itemText = "TRIGGER: " + String(triggerMode == 0 ? "AUTO" : "NORM"); break;
      case 6:  itemText = "COUPLING: " + String(couplingMode == 0 ? "DC" : "AC"); break;
      case 7:  itemText = "V-DIV: " + String(vDivIndex == 0 ? "0.5V" : vDivIndex == 1 ? "1.0V" : "2.0V"); break;
      case 8:  itemText = "THEME: " + String(themeNames[themeColorIdx]); break;
      case 9:  itemText = "BUZZER: " + String(buzzerEnabled ? "ON" : "OFF"); break;
      case 10: itemText = "GEN: " + String(freqNames[testFreqIndex]); break; 
      case 11: itemText = "SCREEN ROT: " + String(rotationNames[screenRotation]); break;
      case 12: itemText = "WATERFALL: " + String(waterfallEn ? "ON" : "OFF"); break;
      case 13: itemText = "LED M: " + String(ledModes[ledMode]); break;
      case 14: itemText = "LED C: " + String(ledColorNames[ledColorIndex]); break;
      case 15: itemText = "BRIGHT: " + String(brightnessNames[brightnessIndex]); break;
      case 16: itemText = "-> LISSAJOUS (XY) MODE"; break;
      case 17: itemText = "-> I2C RADAR SCANNER"; break;
      case 18: itemText = "-> PWM/SERVO TOOL"; break;
      case 19: itemText = "🥟 KHINKALI COOKER"; break;
      case 20: itemText = "[ SAVE & EXIT ]"; break;
    }
    menuSpr.drawString(itemText, 25, (int)yTextPos + 2, 2);
  }
  menuSpr.pushSprite(0, 40);
}

void drawGrid() { 
  int yTop = 40, yBottom = tft.height() - 45, h = yBottom - yTop;
  uint16_t gridColor = (themeColorIdx == 4) ? tft.color565(30, 30, 30) : tft.color565(40, 40, 50);
  if(themeColorIdx == 3) gridColor = tft.color565(0, 45, 10);
  for (int i = 1; i < 4; i++) tft.drawFastHLine(0, yTop + (h * i) / 4, tft.width(), gridColor);
  for (int i = 1; i < 8; i++) tft.drawFastVLine((tft.width() * i) / 8, yTop, h, gridColor);
}

void drawOscilloscope() {
  int yTop = 40; 
  int yBottom = tft.height() - 45;
  int h = yBottom - yTop;
  bool anyActive = false;
  
  for (int i = 0; i < NUM_ADC_PINS; i++) if (pinEnabled[i]) anyActive = true;
  if (!anyActive) {
    if (xPos == 0) { 
      tft.fillRect(0, yTop, tft.width(), h, TFT_BLACK);
      if (useGrid) drawGrid();
      tft.setTextColor(TFT_RED, TFT_BLACK); 
      tft.drawString("NO ACTIVE CH", tft.width()/2 - 40, yTop + h/2, 2); 
    }
    xPos++;
    if (xPos >= tft.width()) xPos = 0; 
    delay(10); 
    return;
  }
  
  int primaryIdx = 0;
  for (int i = 0; i < NUM_ADC_PINS; i++) { 
    if (pinEnabled[i]) { primaryIdx = i; break; } 
  }
  
  // Ждущий режим (Триггер)
  if (xPos == 0 && triggerMode == 1) {
    int tempMin = 4095, tempMax = 0;
    for(int i = 0; i < 50; i++) { 
      int val = analogRead(adcPins[primaryIdx]);
      if (val > tempMax) tempMax = val;
      if (val < tempMin) tempMin = val;
    }
    if ((tempMax - tempMin) < 150) { delay(10); return; }
  }

  // СТАРТ НОВОГО КАДРА И ПОЛНОЕ ОБНОВЛЕНИЕ ЭКРАНА
  if (xPos == 0) {
    unsigned long sweepDuration = millis() - sweepStartTime;
    if (sweepDuration > 0 && (lastMaxAdc - lastMinAdc) > 250 && crossings > 0) 
      freqHz = (crossings * 1000.0) / sweepDuration;
    else freqHz = 0.0; 
    
    vMax = (lastMaxAdc * 3.3) / 4095.0; 
    vMin = (lastMinAdc * 3.3) / 4095.0;
    vPP = vMax - vMin; if(vPP<0) vPP=0;
    
    long totalBuf = 0; 
    for(int b=0; b<WAVE_BUFFER_SIZE; b++) totalBuf += waveBuffer[b];
    vAvg = ((totalBuf / WAVE_BUFFER_SIZE) * 3.3) / 4095.0;

    lastMinAdc = currentMinAdc; lastMaxAdc = currentMaxAdc; 
    currentMinAdc = 4095;
    currentMaxAdc = 0; 
    crossings = 0; sweepStartTime = millis();
    
    uint16_t bgCol = (themeColorIdx == 3) ? tft.color565(0, 12, 4) : TFT_BLACK;
    
    // ПОЛНАЯ ОЧИСТКА ВСЕГО РАБОЧЕГО ПОЛЯ ОСЦИЛЛОГРАФА
    if (!waterfallEn) {
        tft.fillRect(0, yTop, tft.width(), h, bgCol);
        if (useGrid) drawGrid();
    }
    
    // ВЕРХНЯЯ ПАНЕЛЬ
    tft.fillRect(0, 0, tft.width(), 38, TFT_BLACK);
    tft.setTextColor(themeColors[themeColorIdx], TFT_BLACK); 
    char topText[64];
    snprintf(topText, sizeof(topText), "⚡ FREQ: %.1f%s | TRIG: %s", freqHz<1000?freqHz:freqHz/1000.0, freqHz<1000?"Hz":"kHz", triggerMode==0?"AUTO":"NORM");
    tft.drawString(topText, 10, 10, 2);
    tft.drawFastHLine(0, 38, tft.width(), themeColors[themeColorIdx]);
    
    // HUD ПАНЕЛЬ
    int hudY = tft.height() - 40;
    tft.fillRect(0, hudY, tft.width(), 40, TFT_BLACK);
    tft.drawFastHLine(0, hudY, tft.width(), tft.color565(50, 50, 70));
    uint16_t hudTextCol = themeColors[themeColorIdx];
    tft.drawRoundRect(5, hudY+4, 90, 32, 2, tft.color565(40,40,40)); tft.setTextColor(TFT_WHITE);
    tft.drawString("MAX/MIN", 10, hudY+6, 1);
    tft.setTextColor(hudTextCol); char mXT[16]; snprintf(mXT, sizeof(mXT), "^%.1f _%.1f", vMax, vMin);
    tft.drawString(mXT, 10, hudY+18, 1);
    
    tft.drawRoundRect(100, hudY+4, 100, 32, 2, tft.color565(40,40,40)); tft.setTextColor(TFT_WHITE); tft.drawString("Vpp (AMPL)", 105, hudY+6, 1);
    tft.setTextColor(TFT_ORANGE);
    tft.drawString(String(vPP, 2) + " V", 105, hudY+18, 2);

    tft.drawRoundRect(205, hudY+4, 100, 32, 2, tft.color565(40,40,40)); tft.setTextColor(TFT_WHITE); tft.drawString("AVERAGE", 210, hudY+6, 1);
    tft.setTextColor(TFT_MAGENTA);
    tft.drawString(String(vAvg, 2) + " V", 210, hudY+18, 2);
  }
  
  // СЧИТЫВАНИЕ И ОТРИСОВКА
  for (int i = 0; i < NUM_ADC_PINS; i++) {
    if (!pinEnabled[i]) continue;
    int adcValue = 0, samples = (smoothLevel == 1) ? 5 : (smoothLevel == 2) ? 20 : (smoothLevel == 3) ? 50 : 1; 
    
    if (samples > 1) { 
      long adcTotal = 0;
      for (int s = 0; s < samples; s++) adcTotal += analogRead(adcPins[i]); 
      adcValue = adcTotal / samples;
    } 
    else {
      adcValue = analogRead(adcPins[i]);
    }

    if (i == primaryIdx) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      waveBuffer[waveBufferIdx] = adcValue;
      waveBufferIdx = (waveBufferIdx + 1) % WAVE_BUFFER_SIZE; 
      xSemaphoreGive(dataMutex);

      if (adcValue > currentMaxAdc) currentMaxAdc = adcValue;
      if (adcValue < currentMinAdc) currentMinAdc = adcValue; 
      
      int range = (lastMaxAdc - lastMinAdc);
      int midpoint = (lastMinAdc + lastMaxAdc) / 2;
      int hysteresis = (range / 8 < 40) ? 40 : range / 8; 
      
      if (range > 250) { 
        if (!wasAboveMidpoint && (adcValue > (midpoint + hysteresis))) { 
          crossings++;
          wasAboveMidpoint = true; 
        } 
        else if (wasAboveMidpoint && (adcValue < (midpoint - hysteresis))) {
          wasAboveMidpoint = false;
        }
      }
      voltage = (adcValue * 3.3) / 4095.0;
    }
    
    if (couplingMode == 1) adcValue = (adcValue - ((lastMinAdc + lastMaxAdc) / 2)) + 2048;
    int zoomedAdc = adcValue;
    if (vDivIndex == 0) zoomedAdc = (zoomedAdc - 2048) * 2 + 2048;
    else if (vDivIndex == 2) zoomedAdc = (zoomedAdc - 2048) / 2 + 2048;
    if (zoomLevels[zoomIndex] > 1) zoomedAdc = (zoomedAdc - 2048) * zoomLevels[zoomIndex] + 2048; 
    if (zoomedAdc > 4095) zoomedAdc = 4095;
    if (zoomedAdc < 0) zoomedAdc = 0; 
    
    int yPos = map(zoomedAdc, 0, 4095, yBottom, yTop);
    
    uint16_t lineCol = avaliableColors[pinColorIndices[i]];
    if (themeColorIdx == 3) lineCol = tft.color565(0, 255, 50);
    if (themeColorIdx == 4) lineCol = TFT_WHITE;
    
    if (showReference && i == primaryIdx) {
        int refY = map(refBuffer[(waveBufferIdx + xPos) % WAVE_BUFFER_SIZE], 0, 4095, yBottom, yTop);
        tft.drawPixel(xPos, refY, tft.color565(100,100,100));
    }

    // ИСПРАВЛЕНИЕ: Если xPos == 0 (старт нового кадра), мы просто запоминаем 
    // начальную точку. Линию от старого кадра назад рисовать НЕЛЬЗЯ!
    if (xPos == 0) {
        prevYPins[i] = yPos;
    } else {
        tft.drawLine(prevX, prevYPins[i], xPos, yPos, lineCol);
        prevYPins[i] = yPos;
    }
  }
  
  prevX = xPos;
  xPos++; 
  if (xPos >= tft.width()) xPos = 0;
  if (graphSpeed > 0) delay(graphSpeed);
}

// --- ИНСТРУМЕНТЫ ---
void runI2CRadar() {
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_CYAN); tft.drawString("I2C RADAR SCANNING...", 20, 20, 2);
  int found = 0;
  for(byte address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    int x = 10 + (address % 8) * 35;
    int y = 60 + (address / 8) * 20;
    if (error == 0) {
      tft.setTextColor(TFT_GREEN); tft.drawString(String(address, HEX), x, y, 2); found++;
    } else {
      tft.setTextColor(tft.color565(40,40,40)); tft.drawString("..", x, y, 2);
    }
  }
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("FOUND: " + String(found) + " DEVICES", 20, tft.height()-30, 2);
  delay(2000); currentMode = 1; initMenuScreen();
}

void runPWMTool() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_MAGENTA); tft.drawString("PWM & SERVO TOOL", 40, 20, 2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("DUTY CYCLE: " + String(pwmDuty) + "%", 40, 80, 4);
  tft.drawString("FREQ: " + String(testFreqs[testFreqIndex]) + " Hz", 40, 120, 2);
  tft.setTextColor(TFT_GREEN); tft.drawString("[LEFT/RIGHT] TO CHANGE", 20, 180, 2);
  tft.drawString("[OK] TO EXIT", 60, 210, 2);
  
  uint32_t freq = testFreqs[testFreqIndex];
  uint8_t bits = (freq <= 100) ? 14 : (freq <= 10000) ? 10 : 8;
  uint32_t maxDuty = (1 << bits) - 1;
  uint32_t dutyVal = (maxDuty * pwmDuty) / 100;
  #if ESP_IDF_VERSION_MAJOR >= 5
    ledcWrite(TEST_WAVE_PIN, dutyVal);
  #else
    ledcWrite(0, dutyVal);
  #endif
  delay(100);
}

void drawLissajous() {
  int xVal = analogRead(adcPins[0]);
  int yVal = analogRead(adcPins[1]); 
  int drawX = map(xVal, 0, 4095, 0, tft.width());
  int drawY = map(yVal, 0, 4095, tft.height(), 0);
  if (xPos == 0) tft.fillScreen(TFT_BLACK);
  xPos++; if(xPos > 500) xPos = 0;
  
  tft.fillCircle(drawX, drawY, 2, TFT_MAGENTA);
  delay(graphSpeed);
}

// --- УПРАВЛЕНИЕ КНОПКАМИ ---
void handleButtons() {
  bool rUP = digitalRead(BTN_UP), rDOWN = digitalRead(BTN_DOWN), rLEFT = digitalRead(BTN_LEFT), rRIGHT = digitalRead(BTN_RIGHT), rOK = digitalRead(BTN_OK);
  if (rOK == LOW && lastStateOK == HIGH) {
    beep(2000, 30); delay(30);
    if (currentMode == 0) { currentMode = 1; menuCursor = 0; menuScrollOffset = 0; initMenuScreen(); }
    else if (currentMode == 1) {
      if (menuCursor == 0) { currentMode = 4; pinsSubCursor = 0; tft.fillScreen(TFT_BLACK); }
      else if (menuCursor == 16) { currentMode = 8; tft.fillScreen(TFT_BLACK); } 
      else if (menuCursor == 17) { currentMode = 6; } 
      else if (menuCursor == 18) { currentMode = 7; } 
      else if (menuCursor == 19) { currentMode = 5; khinkaliTimerSec = 420; khinkaliRunning = true; } 
      else if (menuCursor == 20) { currentMode = 0; saveSettings(); tft.fillScreen(TFT_BLACK); xPos = 0; }
    }
    else if (currentMode == 4) { if (pinsSubCursor == 3) { currentMode = 1; initMenuScreen(); } }
    else if (currentMode == 7) { currentMode = 1; initMenuScreen(); } 
    else if (currentMode == 8) { currentMode = 1; initMenuScreen(); } 
  } lastStateOK = rOK;
  
  if (rUP == LOW && lastStateUP == HIGH) {
    beep(2000, 30); delay(30);
    if (currentMode == 0) isFrozen = !isFrozen;
    else if (currentMode == 1) { menuCursor--; if (menuCursor < 0) menuCursor = MENU_ITEMS_COUNT - 1; }
    else if (currentMode == 4) { pinsSubCursor--; if (pinsSubCursor < 0) pinsSubCursor = 3; }
  } lastStateUP = rUP;
  if (rDOWN == LOW && lastStateDOWN == HIGH) {
    beep(2000, 30); delay(30);
    if (currentMode == 0) { 
      showReference = !showReference;
      if(showReference) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        for(int i=0; i<WAVE_BUFFER_SIZE; i++) refBuffer[i] = waveBuffer[i];
        xSemaphoreGive(dataMutex);
      }
    }
    else if (currentMode == 1) { menuCursor++; if (menuCursor >= MENU_ITEMS_COUNT) menuCursor = 0; }
    else if (currentMode == 4) { pinsSubCursor++; if (pinsSubCursor > 3) pinsSubCursor = 0; }
  } lastStateDOWN = rDOWN;
  if (rRIGHT == LOW && lastStateRIGHT == HIGH) {
    beep(2000, 30); delay(30);
    if (currentMode == 0) { graphSpeed += 5; if (graphSpeed > 30) graphSpeed = 30; }
    else if (currentMode == 1) {
      switch(menuCursor) {
        case 1: zoomIndex = (zoomIndex + 1) % 4; break;
        case 2: smoothLevel = (smoothLevel + 1) % 4; break;
        case 3: graphSpeed = (graphSpeed + 5) % 35; break;
        case 4: useGrid = !useGrid; break;
        case 5: triggerMode = (triggerMode + 1) % 2; break;
        case 6: couplingMode = (couplingMode + 1) % 2; break;
        case 7: vDivIndex = (vDivIndex + 1) % 3; break;
        case 8: themeColorIdx = (themeColorIdx + 1) % 5; initMenuScreen(); break;
        case 9: buzzerEnabled = !buzzerEnabled; break;
        case 10: testFreqIndex = (testFreqIndex + 1) % NUM_FREQS; generatorChanged = true; break;
        case 11: screenRotation = (screenRotation + 1) % 4; tft.setRotation(screenRotation); menuSpr.deleteSprite(); menuSpr.createSprite(tft.width(), 160); initMenuScreen(); break;
        case 12: waterfallEn = !waterfallEn; break;
        case 13: ledMode = (ledMode + 1) % 4; updateLED(); break;
        case 14: ledColorIndex = (ledColorIndex + 1) % 7; updateLED(); break;
        case 15: brightnessIndex = (brightnessIndex + 1) % 3; updateBrightness(); break;
      }
    }
    else if (currentMode == 4) { if (pinsSubCursor < 3) pinEnabled[pinsSubCursor] = !pinEnabled[pinsSubCursor]; }
    else if (currentMode == 7) { pwmDuty += 5; if(pwmDuty>100) pwmDuty=100; } 
  } lastStateRIGHT = rRIGHT;
  
  if (rLEFT == LOW && lastStateLEFT == HIGH) {
    beep(2000, 30); delay(30);
    if (currentMode == 0) { graphSpeed -= 5; if (graphSpeed < 0) graphSpeed = 0; } 
    else if (currentMode == 1) {
      switch(menuCursor) {
        case 1: zoomIndex = (zoomIndex - 1 + 4) % 4; break;
        case 2: smoothLevel = (smoothLevel - 1 + 4) % 4; break;
        case 3: graphSpeed -= 5; if(graphSpeed < 0) graphSpeed = 30; break;
        case 4: useGrid = !useGrid; break;
        case 5: triggerMode = (triggerMode - 1 + 2) % 2; break;
        case 6: couplingMode = (couplingMode - 1 + 2) % 2; break;
        case 7: vDivIndex = (vDivIndex - 1 + 3) % 3; break;
        case 8: themeColorIdx = (themeColorIdx - 1 + 5) % 5; initMenuScreen(); break;
        case 9: buzzerEnabled = !buzzerEnabled; break;
        case 10: testFreqIndex = (testFreqIndex - 1 + NUM_FREQS) % NUM_FREQS; generatorChanged = true; break;
        case 11: screenRotation = (screenRotation - 1 + 4) % 4; tft.setRotation(screenRotation); menuSpr.deleteSprite(); menuSpr.createSprite(tft.width(), 160); initMenuScreen(); break;
        case 12: waterfallEn = !waterfallEn; break;
        case 13: ledMode = (ledMode - 1 + 4) % 4; updateLED(); break;
        case 14: ledColorIndex = (ledColorIndex - 1 + 7) % 7; updateLED(); break;
        case 15: brightnessIndex = (brightnessIndex - 1 + 3) % 3; updateBrightness(); break;
      }
    }
    else if (currentMode == 4) { if (pinsSubCursor < 3) pinColorIndices[pinsSubCursor] = (pinColorIndices[pinsSubCursor] + 1) % 7; }
    else if (currentMode == 7) { pwmDuty -= 5; if(pwmDuty<0) pwmDuty=0; } 
  } lastStateLEFT = rLEFT;
}

// --- УТИЛИТЫ ---
void updateGenerator() {
  uint32_t freq = testFreqs[testFreqIndex];
  uint8_t bits = (freq <= 100) ? 14 : (freq <= 10000) ? 10 : 8;
  uint32_t duty = (1 << bits) / 2; 
  #if ESP_IDF_VERSION_MAJOR >= 5
    ledcDetach(TEST_WAVE_PIN); delay(5); ledcAttach(TEST_WAVE_PIN, freq, bits);
    delay(10); ledcWrite(TEST_WAVE_PIN, duty);
  #else
    ledcSetup(0, freq, bits); ledcAttachPin(TEST_WAVE_PIN, 0); delay(10); ledcWrite(0, duty);
  #endif
}

// УЛУЧШЕННАЯ ЭНЕРГОНЕЗАВИСИМАЯ ПАМЯТЬ НАСТРОЕК
void loadSettings() {
  prefs.begin("oscin", true);
  graphSpeed = prefs.getInt("speed", 10); 
  zoomIndex = prefs.getInt("zoom_idx", 0);
  themeColorIdx = prefs.getInt("theme", 0);
  screenRotation = prefs.getInt("screen_rot", 1);
  waterfallEn = prefs.getBool("waterfall", false);
  
  // Новые сохраняемые параметры:
  smoothLevel = prefs.getInt("smooth", 2);
  triggerMode = prefs.getInt("trig_mode", 0);
  couplingMode = prefs.getInt("coupling", 0);
  vDivIndex = prefs.getInt("v_div", 1);
  buzzerEnabled = prefs.getBool("buzzer", false);
  testFreqIndex = prefs.getInt("test_freq", 2);
  ledMode = prefs.getInt("led_mode", 2);
  ledColorIndex = prefs.getInt("led_color", 0);
  brightnessIndex = prefs.getInt("bright_idx", 0);
  useGrid = prefs.getBool("use_grid", true);
  
  pinEnabled[0] = prefs.getBool("pin_en_0", true);
  pinEnabled[1] = prefs.getBool("pin_en_1", true);
  pinEnabled[2] = prefs.getBool("pin_en_2", true);
  
  pinColorIndices[0] = prefs.getInt("pin_col_0", 5);
  pinColorIndices[1] = prefs.getInt("pin_col_1", 2);
  pinColorIndices[2] = prefs.getInt("pin_col_2", 6);
  prefs.end();
}

void saveSettings() {
  prefs.begin("oscin", false); 
  prefs.putInt("speed", graphSpeed); 
  prefs.putInt("zoom_idx", zoomIndex);
  prefs.putInt("theme", themeColorIdx); 
  prefs.putInt("screen_rot", screenRotation);
  prefs.putBool("waterfall", waterfallEn);
  
  // Сохраняем всё остальное:
  prefs.putInt("smooth", smoothLevel);
  prefs.putInt("trig_mode", triggerMode);
  prefs.putInt("coupling", couplingMode);
  prefs.putInt("v_div", vDivIndex);
  prefs.putBool("buzzer", buzzerEnabled);
  prefs.putInt("test_freq", testFreqIndex);
  prefs.putInt("led_mode", ledMode);
  prefs.putInt("led_color", ledColorIndex);
  prefs.putInt("bright_idx", brightnessIndex);
  prefs.putBool("use_grid", useGrid);
  
  prefs.putBool("pin_en_0", pinEnabled[0]);
  prefs.putBool("pin_en_1", pinEnabled[1]);
  prefs.putBool("pin_en_2", pinEnabled[2]);
  
  prefs.putInt("pin_col_0", pinColorIndices[0]);
  prefs.putInt("pin_col_1", pinColorIndices[1]);
  prefs.putInt("pin_col_2", pinColorIndices[2]);
  prefs.end();
}

void updatePinsMenuDisplay() {
  float targetVisualY = (pinsSubCursor * 30.0) + 18.0;
  channelsVisualY += (targetVisualY - channelsVisualY) * 0.30;

  menuSpr.fillSprite(TFT_BLACK);
  menuSpr.fillRect(10, (int)channelsVisualY, menuSpr.width() - 20, 26, menuSpr.color565(30, 60, 50));
  for (int i = 0; i < 4; i++) {
    float yTextPos = 22.0 + (i * 30.0);
    if (i == pinsSubCursor) menuSpr.setTextColor(TFT_GREEN, menuSpr.color565(30, 60, 50));
    else menuSpr.setTextColor(TFT_WHITE, TFT_BLACK);
    if (i < 3) {
      String name = (i==0)?"CH4 ":(i==1)?"CH5 ":"CH9 ";
      String status = pinEnabled[i] ? "[ON]" : "[OFF]";
      String colName = colorNames[pinColorIndices[i]];
      menuSpr.drawString(name + status + " | COLOR: " + colName, 25, (int)yTextPos, 2);
    } else {
      menuSpr.drawString("[ BACK ]", 25, (int)yTextPos, 2);
    }
  }
  menuSpr.pushSprite(0, 40);
}

void updateWiFiMenuDisplay() {
  tft.fillScreen(TFT_BLACK); 
  tft.setTextColor(themeColors[themeColorIdx], TFT_BLACK); 
  tft.drawString("=== WI-FI CONTROL ===", 70, 15, 2);
  tft.drawFastHLine(0, 40, tft.width(), themeColors[themeColorIdx]);
  if (wifiSubCursor == 0) { 
    tft.fillRect(20, 60, tft.width()-40, 30, tft.color565(0,60,60)); 
    tft.setTextColor(TFT_GREEN, tft.color565(0,60,60));
  }
  else tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  tft.drawString(wifiEnabled ? "MODEM: ACTIVE" : "MODEM: DISABLED", 30, 67, 2);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("SSID: ESP32-Scope", 30, 110, 2);
  tft.drawString("PASS: 12345678", 30, 135, 2);
  tft.setTextColor(tft.color565(0, 255, 150), TFT_BLACK);
  tft.drawString("IP:   192.168.4.1", 30, 160, 2);

  if (wifiSubCursor == 1) { 
    tft.fillRect(20, 195, tft.width()-40, 30, tft.color565(40,40,40));
    tft.setTextColor(TFT_GREEN, tft.color565(40,40,40)); 
  }
  else tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  tft.drawString("[ BACK TO MENU ]", 30, 202, 2);
}

void toggleWiFi() { 
  wifiEnabled = !wifiEnabled; 
  if (wifiEnabled) { WiFi.softAP("ESP32-Scope", "12345678"); server.begin(); } 
  else { WiFi.softAPdisconnect(true); } 
  saveSettings(); 
}

void drawKhinkaliScreen() {
  if (khinkaliRunning && millis() - lastKhinkaliSecTick >= 1000) {
    lastKhinkaliSecTick = millis();
    if (khinkaliTimerSec > 0) {
      khinkaliTimerSec--;
      steamAnimFrame = (steamAnimFrame + 1) % 3;
    }
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("🥟 KHINKALI COOKING PROFILE 🥟", 35, 15, 2);
  tft.drawFastHLine(0, 40, tft.width(), TFT_YELLOW);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (khinkaliRunning && khinkaliTimerSec > 0) {
    if (steamAnimFrame == 0) tft.drawString("  ~   ~   ~  ", 130, 70, 2);
    else if (steamAnimFrame == 1) tft.drawString("   ~   ~   ~ ", 130, 70, 2);
    else tft.drawString(" ~   ~   ~   ", 130, 70, 2);
  }
  tft.setTextColor(tft.color565(200, 180, 140), TFT_BLACK);
  tft.drawString("    ( ( 🥟 ) )    ", 110, 100, 2);
  int mins = khinkaliTimerSec / 60; int secs = khinkaliTimerSec % 60;
  char timeStr[16]; snprintf(timeStr, sizeof(timeStr), "%02d:%02d", mins, secs);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); 
  tft.drawString("TIME REMAINING:", 60, 160, 2);
  
  if (khinkaliTimerSec == 0 && khinkaliRunning) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("!! READY !!", 210, 160, 2);
    if (millis() % 1000 < 500) beep(1000, 200);
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.drawString(timeStr, 220, 160, 2);
  }
}

void updateLED() { 
  if (ledMode == 0) { led.setPixelColor(0, 0); led.show(); } 
  else if (ledMode == 1) { led.setPixelColor(0, led.Color(ledRGBValues[ledColorIndex][0], ledRGBValues[ledColorIndex][1], ledRGBValues[ledColorIndex][2])); led.show(); } 
}

void handleLEDAnimations() {
  if (ledMode < 2) return;
  if (millis() - lastLedUpdate >= 20) {
    lastLedUpdate = millis();
    if (ledMode == 2) { led.setPixelColor(0, ledWheel(rainbowHue)); led.show(); rainbowHue += 2; } 
    else if (ledMode == 3) {
      sweepVal += sweepDirection * 4;
      if (sweepVal >= 255) { sweepVal = 255; sweepDirection = -1; } 
      if (sweepVal <= 10) { sweepVal = 10; sweepDirection = 1; }
      led.setPixelColor(0, led.Color((ledRGBValues[ledColorIndex][0]*sweepVal)/255, (ledRGBValues[ledColorIndex][1]*sweepVal)/255, (ledRGBValues[ledColorIndex][2]*sweepVal)/255)); led.show();
    }
  }
}

uint32_t ledWheel(uint8_t wheelPos) { 
  wheelPos = 255 - wheelPos;
  if (wheelPos < 85) return led.Color(255 - wheelPos * 3, 0, wheelPos * 3);
  else if (wheelPos < 170) { wheelPos -= 85; return led.Color(0, wheelPos * 3, 255 - wheelPos * 3); } 
  else { wheelPos -= 170; return led.Color(wheelPos * 3, 255 - wheelPos * 3, 0); } 
}

void updateBrightness() { 
  #if ESP_IDF_VERSION_MAJOR >= 5
    ledcWrite(TFT_BL_PIN, brightnessLevels[brightnessIndex]);
  #else
    ledcWrite(1, brightnessLevels[brightnessIndex]); 
  #endif
}
