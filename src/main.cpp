#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

#define MAX_STOCKS 20

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);

struct StockData {
  char symbol[10];
  char name[20];
  float price;
  float change_percent;
  bool active;
};

StockData stocks[MAX_STOCKS];
int pageIntervalSec = 5;       // 預設頁面輪播時間 (秒)
int totalActiveStocks = 0;
int currentPage = 0;
unsigned long lastPageSwitch = 0;

// 函式宣告
void loadConfig();
void saveConfig();
void countActiveStocks();
void drawStockPage(int pageIndex);
void handleRoot();
void handleSave();

// ---------------------------------------------------------------------------
// LittleFS 檔案讀寫 (ArduinoJson v7 格式)
// ---------------------------------------------------------------------------
void loadConfig() {
  if (!LittleFS.exists("/config.json")) {
    strcpy(stocks[0].symbol, "2330"); strcpy(stocks[0].name, "2330"); stocks[0].price = 2425.0; stocks[0].change_percent = 1.04;
    strcpy(stocks[1].symbol, "2454"); strcpy(stocks[1].name, "2454"); stocks[1].price = 4235.0; stocks[1].change_percent = 6.94;
    strcpy(stocks[2].symbol, "0050"); strcpy(stocks[2].name, "0050"); stocks[2].price = 108.65; stocks[2].change_percent = -1.92;
    strcpy(stocks[3].symbol, "2317"); strcpy(stocks[3].name, "2317"); stocks[3].price = 205.5; stocks[3].change_percent = 2.49;
    return;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (!error) {
    pageIntervalSec = doc["interval"] | 5;
    JsonArray array = doc["stocks"];
    for (size_t i = 0; i < MAX_STOCKS && i < array.size(); i++) {
      strncpy(stocks[i].symbol, array[i]["sym"] | "", sizeof(stocks[i].symbol));
      strncpy(stocks[i].name, array[i]["name"] | "", sizeof(stocks[i].name));
    }
  }
}

void saveConfig() {
  JsonDocument doc;
  doc["interval"] = pageIntervalSec;
  JsonArray array = doc["stocks"].to<JsonArray>();

  for (int i = 0; i < MAX_STOCKS; i++) {
    JsonObject obj = array.add<JsonObject>();
    obj["sym"] = stocks[i].symbol;
    obj["name"] = stocks[i].name;
  }

  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
  }
}

// ---------------------------------------------------------------------------
// Web Server 網頁設定介面
// ---------------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP8266 股票看板設定</title>";
  html += "<style>";
  html += "body { font-family: sans-serif; background: #121212; color: #fff; padding: 15px; }";
  html += ".card { background: #1e1e1e; padding: 15px; border-radius: 8px; max-width: 480px; margin: auto; }";
  html += "h2 { color: #4CAF50; text-align: center; margin-bottom: 15px; }";
  html += ".group { background: #252525; padding: 12px; border-radius: 6px; margin-bottom: 15px; }";
  html += ".row { display: flex; gap: 8px; margin-bottom: 8px; align-items: center; }";
  html += ".num { width: 25px; color: #888; text-align: right; }";
  html += "label { display: block; color: #aaa; margin-bottom: 5px; font-size: 14px; }";
  html += "input[type='text'], input[type='number'] { flex: 1; padding: 6px; background: #2a2a2a; border: 1px solid #444; color: #fff; border-radius: 4px; }";
  html += ".btn { width: 100%; padding: 12px; background: #4CAF50; border: none; color: white; font-size: 16px; margin-top: 15px; border-radius: 5px; cursor: pointer; }";
  html += "</style></head><body>";

  html += "<div class='card'><h2>📈 股票顯示器設定</h2>";
  html += "<form action='/save' method='POST'>";

  html += "<div class='group'><label><b>⏱️ 頁面輪播時間 (秒)：</b></label>";
  html += "<input type='number' name='interval' value='" + String(pageIntervalSec) + "' min='1' max='60' required></div>";

  html += "<div class='group'><label><b>📋 股票清單 (最多 20 支)：</b></label>";
  for (int i = 0; i < MAX_STOCKS; i++) {
    html += "<div class='row'>";
    html += "<span class='num'>" + String(i + 1) + ".</span>";
    html += "<input type='text' name='sym" + String(i) + "' value='" + String(stocks[i].symbol) + "' placeholder='代碼'>";
    html += "<input type='text' name='name" + String(i) + "' value='" + String(stocks[i].name) + "' placeholder='名稱'>";
    html += "</div>";
  }
  html += "</div>";

  html += "<input type='submit' value='💾 儲存並套用重啟' class='btn'>";
  html += "</form></div></body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("interval")) {
    pageIntervalSec = server.arg("interval").toInt();
    if (pageIntervalSec < 1) pageIntervalSec = 5;
  }

  for (int i = 0; i < MAX_STOCKS; i++) {
    if (server.hasArg("sym" + String(i))) strncpy(stocks[i].symbol, server.arg("sym" + String(i)).c_str(), sizeof(stocks[i].symbol));
    if (server.hasArg("name" + String(i))) strncpy(stocks[i].name, server.arg("name" + String(i)).c_str(), sizeof(stocks[i].name));
  }

  saveConfig();

  String msg = "<html><body style='background:#121212;color:#fff;text-align:center;'>";
  msg += "<h2>設定已儲存！</h2><p>ESP8266 正在重啟...</p></body></html>";
  server.send(200, "text/html", msg);
  
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// TFT_eSPI 繪圖邏輯
// ---------------------------------------------------------------------------
void countActiveStocks() {
  totalActiveStocks = 0;
  for (int i = 0; i < MAX_STOCKS; i++) {
    if (strlen(stocks[i].symbol) > 0) {
      stocks[i].active = true;
      totalActiveStocks++;
    } else {
      stocks[i].active = false;
    }
  }
}

void drawStockPage(int pageIndex) {
  tft.fillScreen(TFT_BLACK);

  int totalPages = (totalActiveStocks + 3) / 4;
  if (totalPages == 0) totalPages = 1;

  // 左側垂直標籤[cite: 5]
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("STOCKS", 5, 100, 2);

  char pageStr[10];
  sprintf(pageStr, "%d/%d", pageIndex + 1, totalPages);
  tft.drawString(pageStr, 5, 120, 2);

  int startIdx = pageIndex * 4;
  int startY = 10;
  int rowHeight = 58;

  for (int i = 0; i < 4; i++) {
    int stockIdx = startIdx + i;
    if (stockIdx >= totalActiveStocks) break;

    int y = startY + (i * rowHeight);

    // 第一行：代碼與價格
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(stocks[stockIdx].symbol, 40, y, 4); // 字型 4 (清晰英數)

    char priceStr[10];
    dtostrf(stocks[stockIdx].price, 4, (stocks[stockIdx].price >= 1000) ? 1 : 2, priceStr);
    tft.drawString(priceStr, 130, y, 4);

    // 第二行：名稱與漲跌幅[cite: 5]
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString(stocks[stockIdx].name, 40, y + 26, 2);

    uint16_t color = (stocks[stockIdx].change_percent >= 0) ? TFT_RED : TFT_GREEN;
    char changeStr[15];
    if (stocks[stockIdx].change_percent >= 0) {
      sprintf(changeStr, "+%.2f%%", stocks[stockIdx].change_percent);
    } else {
      sprintf(changeStr, "%.2f%%", stocks[stockIdx].change_percent);
    }

    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(changeStr, 140, y + 26, 2);

    if (i < 3 && (stockIdx + 1) < totalActiveStocks) {
      tft.drawFastHLine(35, y + 50, 200, TFT_DARKGREY);
    }
  }
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // 初始化 LittleFS
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  countActiveStocks();

  // 初始化 TFT_eSPI 螢幕
  tft.init();
  tft.setRotation(3); // 側向方向[cite: 5]
  tft.fillScreen(TFT_BLACK);

  // 1. 使用 WiFiManager 配網 (若連不上 WiFi 會開 AP 供連線)
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ESP8266-Stock-Ticker")) {
    Serial.println("WiFi 配網逾時，進入獨立模式");
  }

  // 2. 啟動 Web Server (連上 WiFi 後可輸入 ESP8266 的 IP 進入設定)
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  drawStockPage(0);
}

void loop() {
  server.handleClient();

  unsigned long intervalMs = pageIntervalSec * 1000;
  if (millis() - lastPageSwitch >= intervalMs) {
    lastPageSwitch = millis();

    int totalPages = (totalActiveStocks + 3) / 4;
    if (totalPages > 0) {
      currentPage = (currentPage + 1) % totalPages;
    } else {
      currentPage = 0;
    }

    drawStockPage(currentPage);
  }
}
