// StockTV - ESP8266 + ST7789 240x240 (SmallTV style)
// 直接從台灣證交所 mis API 抓股價，不需要橋接程式
// 20 檔自選股、每頁 4 檔自動翻頁、網頁設定 (http://stocktv.local 或螢幕上顯示的 IP)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

// ---------------- 可調整常數 ----------------
#define MAX_STOCKS   20
#define PER_PAGE     4
#define BATCH        5          // 每次 API 請求查幾檔 (每檔會同時查 tse/otc)
#define HEADER_H     20
#define ROW_H        55         // 20 + 4*55 = 240
#define BL_PIN       5          // SmallTV 背光腳位
#define BL_ACTIVE_LOW 1         // SmallTV 為低電位點亮；若螢幕全黑可改成 0
#define TWSE_HOST    "mis.twse.com.tw"

// ---------------- 設定 ----------------
struct Config {
  String codes = "2330,2317,2454,2412,0050,2881";
  int pageSec = 5;       // 每頁停留秒數
  int refreshSec = 30;   // 開盤時更新股價的間隔
  int bright = 80;       // 亮度 5~100
  int rot = 0;           // 螢幕旋轉 0~3
  bool upRed = true;     // true = 台股習慣 (紅漲綠跌)
} cfg;

TFT_eSPI tft;
ESP8266WebServer server(80);

struct Quote {
  float price = 0;
  float prev = 0;
  bool valid = false;
};
char codes[MAX_STOCKS][8];
Quote quotes[MAX_STOCKS];
int nStocks = 0;
int curPage = 0;
int pages = 1;

bool pendingApply = false;
bool needRefresh = true;
bool lastOk = true;
bool everRefreshed = false;
bool useMfln = false;
uint32_t lastRefresh = 0, lastPage = 0;
int lastMinute = -1;

// ---------------- 工具 ----------------
bool timeOk() { return time(nullptr) > 1700000000; }

bool marketOpen() {
  if (!timeOk()) return true;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  return m >= 8 * 60 + 55 && m <= 13 * 60 + 35;
}

String sanitizeCodes(const String &in) {
  String out, tok;
  int n = 0;
  String src = in + "\n";
  for (size_t i = 0; i < src.length(); i++) {
    char c = src[i];
    if (isalnum((unsigned char)c)) {
      tok += (char)toupper((unsigned char)c);
    } else {
      if (tok.length() >= 4 && tok.length() <= 6 && n < MAX_STOCKS) {
        if (out.length()) out += ',';
        out += tok;
        n++;
      }
      tok = "";
    }
  }
  return out;
}

void setBacklight(int pct) {
  pinMode(BL_PIN, OUTPUT);
  analogWriteRange(100);
  analogWrite(BL_PIN, BL_ACTIVE_LOW ? 100 - pct : pct);
}

void saveConfig() {
  JsonDocument d;
  d["codes"] = cfg.codes;
  d["pageSec"] = cfg.pageSec;
  d["refreshSec"] = cfg.refreshSec;
  d["bright"] = cfg.bright;
  d["rot"] = cfg.rot;
  d["upRed"] = cfg.upRed;
  File f = LittleFS.open("/config.json", "w");
  if (f) { serializeJson(d, f); f.close(); }
}

void loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  JsonDocument d;
  if (deserializeJson(d, f)) { f.close(); return; }
  f.close();
  cfg.codes = String(d["codes"] | cfg.codes.c_str());
  cfg.pageSec = d["pageSec"] | cfg.pageSec;
  cfg.refreshSec = d["refreshSec"] | cfg.refreshSec;
  cfg.bright = d["bright"] | cfg.bright;
  cfg.rot = d["rot"] | cfg.rot;
  cfg.upRed = d["upRed"] | cfg.upRed;
  cfg.codes = sanitizeCodes(cfg.codes);
  cfg.pageSec = constrain(cfg.pageSec, 2, 120);
  cfg.refreshSec = constrain(cfg.refreshSec, 10, 600);
  cfg.bright = constrain(cfg.bright, 5, 100);
  cfg.rot = constrain(cfg.rot, 0, 3);
}

// ---------------- 顯示 ----------------
void showMsg(const char *a, const char *b = "", const char *c = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(a, 120, 70, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(b, 120, 115, 2);
  tft.drawString(c, 120, 140, 2);
  tft.setTextDatum(TL_DATUM);
}

uint16_t colorFor(float chg) {
  if (chg > 0.0001f) return cfg.upRed ? TFT_RED : TFT_GREEN;
  if (chg < -0.0001f) return cfg.upRed ? TFT_GREEN : TFT_RED;
  return TFT_WHITE;
}

void fmtPrice(float p, char *buf, size_t n) {
  if (p >= 1000) snprintf(buf, n, "%.1f", p);
  else snprintf(buf, n, "%.2f", p);
}

void drawHeader() {
  const uint16_t bg = 0x1082;
  tft.fillRect(0, 0, 240, HEADER_H, bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  char buf[16];
  if (timeOk()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lastMinute = t.tm_min;
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  tft.drawString(buf, 4, 2, 2);

  tft.setTextDatum(TC_DATUM);
  if (!lastOk) tft.setTextColor(TFT_RED, bg);
  else tft.setTextColor(marketOpen() ? TFT_GREEN : TFT_DARKGREY, bg);
  tft.drawString(!lastOk ? "NET ERR" : (marketOpen() ? "OPEN" : "CLOSED"), 120, 2, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, bg);
  snprintf(buf, sizeof(buf), "%d/%d", curPage + 1, pages);
  tft.drawString(buf, 236, 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawRow(int slot, int idx) {
  int y0 = HEADER_H + slot * ROW_H;
  tft.fillRect(0, y0, 240, ROW_H, TFT_BLACK);
  if (idx >= nStocks) return;
  Quote &q = quotes[idx];
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(codes[idx], 6, y0 + 4, 4);

  tft.setTextDatum(TR_DATUM);
  if (!q.valid) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("--", 234, y0 + 4, 4);
  } else {
    float chg = q.price - q.prev;
    float pct = q.prev > 0 ? chg / q.prev * 100.0f : 0;
    uint16_t col = colorFor(chg);
    char buf[32];
    fmtPrice(q.price, buf, sizeof(buf));
    tft.setTextColor(col, TFT_BLACK);
    tft.drawString(buf, 234, y0 + 4, 4);

    snprintf(buf, sizeof(buf), "%+.2f (%+.2f%%)", chg, pct);
    tft.drawString(buf, 234, y0 + 34, 2);

    char pbuf[16];
    fmtPrice(q.prev, pbuf, sizeof(pbuf));
    snprintf(buf, sizeof(buf), "Y %s", pbuf);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(buf, 6, y0 + 34, 2);
  }
  tft.drawFastHLine(0, y0 + ROW_H - 1, 240, 0x2104);
}

void drawPage() {
  if (nStocks == 0) {
    showMsg("No stocks", "Open the web page", "to add stock codes");
    return;
  }
  drawHeader();
  for (int s = 0; s < PER_PAGE; s++) drawRow(s, curPage * PER_PAGE + s);
}

// ---------------- 抓股價 ----------------
// 證交所 MIS 需要先拜訪 index.jsp 取得 session cookie，API 才會回資料
String twCookie;

bool warmTw() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  if (useMfln) client->setBufferSizes(1024, 1024);
  HTTPClient http;
  const char *keys[] = {"Set-Cookie"};
  http.collectHeaders(keys, 1);
  http.useHTTP10(true);
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(*client, "https://" TWSE_HOST "/stock/index.jsp")) return false;
  int code = http.GET();
  if (code > 0 && http.hasHeader("Set-Cookie")) {
    String s = http.header("Set-Cookie");
    int p = s.indexOf(';');
    twCookie = p > 0 ? s.substring(0, p) : s;
  }
  http.end();
  Serial.printf("warm TWSE: http=%d cookie=%s\n", code, twCookie.c_str());
  return twCookie.length() > 0;
}

bool fetchBatch(int start, int count) {
  String exch;
  for (int i = start; i < start + count; i++) {
    if (i > start) exch += "%7C";
    exch += "tse_" + String(codes[i]) + ".tw%7Cotc_" + String(codes[i]) + ".tw";
  }
  String url = String("https://" TWSE_HOST "/stock/api/getStockInfo.jsp?ex_ch=") + exch + "&json=1&delay=0";

  if (twCookie.length() == 0) warmTw();   // 先暖機，且不與下面的 TLS 連線同時存在以省記憶體

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  if (useMfln) client->setBufferSizes(1024, 1024);

  HTTPClient http;
  http.useHTTP10(true);          // 避免 chunked，才能直接串流解析 JSON
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(*client, url)) return false;
  if (twCookie.length()) http.addHeader("Cookie", twCookie);
  http.addHeader("Referer", "https://" TWSE_HOST "/stock/index.jsp");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP %d, heap=%u\n", code, ESP.getFreeHeap());
    http.end();
    return false;
  }

  JsonDocument filter;
  JsonObject f = filter["msgArray"].add<JsonObject>();
  f["c"] = true; f["z"] = true; f["y"] = true; f["b"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }

  for (JsonObject o : doc["msgArray"].as<JsonArray>()) {
    const char *c = o["c"];
    if (!c) continue;
    for (int i = start; i < start + count; i++) {
      if (strcmp(codes[i], c) != 0) continue;
      float prev = o["y"].is<const char *>() ? atof(o["y"]) : 0;
      float price = 0;
      const char *z = o["z"];
      if (z && z[0] != '-' && atof(z) > 0) {
        price = atof(z);
      } else {
        const char *b = o["b"];             // 尚無成交：用最佳買價 (格式 "590.0_589.0_...")
        if (b && atof(b) > 0) price = atof(b);
        else price = prev;                  // 還是沒有：用昨收
      }
      if (prev <= 0) prev = price;
      quotes[i].price = price;
      quotes[i].prev = prev;
      quotes[i].valid = price > 0;
    }
  }
  return true;
}

void doRefresh() {
  bool ok = true;
  for (int s = 0; s < nStocks; s += BATCH) {
    int c = min(BATCH, nStocks - s);
    if (!fetchBatch(s, c)) ok = false;
    uint32_t t = millis();
    while (millis() - t < 1200) {   // 間隔 1.2 秒避免被證交所限流，期間仍處理網頁請求
      server.handleClient();
      delay(10);
    }
  }
  lastOk = ok;
  everRefreshed = true;
  Serial.printf("refresh done ok=%d heap=%u\n", ok, ESP.getFreeHeap());
}

// ---------------- 套用設定 ----------------
void applyConfig() {
  nStocks = 0;
  String s = cfg.codes + ",";
  int from = 0;
  while (true) {
    int p = s.indexOf(',', from);
    if (p < 0) break;
    String t = s.substring(from, p);
    from = p + 1;
    if (t.length() >= 4 && nStocks < MAX_STOCKS) {
      t.toCharArray(codes[nStocks], 8);
      quotes[nStocks] = Quote();
      nStocks++;
    }
  }
  pages = max(1, (nStocks + PER_PAGE - 1) / PER_PAGE);
  curPage = 0;
  tft.setRotation(cfg.rot);
  setBacklight(cfg.bright);
  tft.fillScreen(TFT_BLACK);
  everRefreshed = false;
  needRefresh = true;
  lastPage = millis();
  drawPage();
}

// ---------------- 網頁 ----------------
String htmlPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>股票小電視</title><style>"
               "body{font-family:sans-serif;max-width:480px;margin:auto;padding:12px}"
               "label{display:block;margin-top:12px;font-weight:bold}"
               "textarea,input,select{width:100%;box-sizing:border-box;padding:6px;font-size:16px}"
               "button{margin-top:16px;padding:10px;width:100%;font-size:16px}"
               "table{width:100%;border-collapse:collapse;margin-top:8px}"
               "td,th{border-bottom:1px solid #ccc;padding:4px;text-align:left}"
               "</style></head><body><h2>股票小電視設定</h2><form method=POST action=/save>");
  h += F("<label>自選股代號（最多 20 檔；逗號、空白或換行分隔；上市上櫃自動判斷）</label>"
         "<textarea name=stocks rows=4>");
  h += cfg.codes;
  h += F("</textarea>");
  h += F("<label>每頁停留秒數（2~120）</label><input type=number name=page min=2 max=120 value=");
  h += cfg.pageSec;
  h += F("><label>股價更新間隔秒數（開盤時，10~600）</label><input type=number name=refresh min=10 max=600 value=");
  h += cfg.refreshSec;
  h += F("><label>亮度（5~100）</label><input type=number name=bright min=5 max=100 value=");
  h += cfg.bright;
  h += F("><label>螢幕方向</label><select name=rot>");
  for (int r = 0; r < 4; r++) {
    h += "<option value=" + String(r) + (cfg.rot == r ? " selected" : "") + ">" + String(r * 90) + "°</option>";
  }
  h += F("</select><label><input type=checkbox name=upred value=1 style='width:auto'");
  if (cfg.upRed) h += " checked";
  h += F("> 紅漲綠跌（台股習慣）</label><button type=submit>儲存</button></form>");

  h += F("<h3>目前報價</h3><table><tr><th>代號</th><th>現價</th><th>昨收</th></tr>");
  for (int i = 0; i < nStocks; i++) {
    h += "<tr><td>" + String(codes[i]) + "</td><td>";
    if (quotes[i].valid) h += String(quotes[i].price, 2) + "</td><td>" + String(quotes[i].prev, 2);
    else h += String(everRefreshed ? "查無資料" : "等待中") + "</td><td>";
    h += "</td></tr>";
  }
  h += F("</table><p><a href=/wifireset onclick=\"return confirm('清除 WiFi 設定並重開機？')\">重設 WiFi</a></p>"
         "</body></html>");
  return h;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", htmlPage()); }

void handleSave() {
  cfg.codes = sanitizeCodes(server.arg("stocks"));
  cfg.pageSec = constrain(server.arg("page").toInt(), 2, 120);
  cfg.refreshSec = constrain(server.arg("refresh").toInt(), 10, 600);
  cfg.bright = constrain(server.arg("bright").toInt(), 5, 100);
  cfg.rot = constrain(server.arg("rot").toInt(), 0, 3);
  cfg.upRed = server.hasArg("upred");
  saveConfig();
  pendingApply = true;   // 由 loop() 套用，避免在抓資料途中改動陣列
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWifiReset() {
  server.send(200, "text/plain; charset=utf-8", "WiFi 設定已清除，裝置重開機中…");
  delay(500);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ---------------- setup / loop ----------------
void apCallback(WiFiManager *) {
  showMsg("Setup WiFi", "Join WiFi: StockTV-Setup", "Open 192.168.4.1");
}

void setup() {
  Serial.begin(115200);
  delay(100);

  if (!LittleFS.begin()) { LittleFS.format(); LittleFS.begin(); }
  loadConfig();

  tft.init();
  tft.setRotation(cfg.rot);
  tft.fillScreen(TFT_BLACK);
  setBacklight(cfg.bright);
  showMsg("StockTV", "Connecting WiFi...");

  WiFiManager wm;
  wm.setAPCallback(apCallback);
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("StockTV-Setup")) ESP.restart();

  String ip = WiFi.localIP().toString();
  showMsg("Connected", ip.c_str(), "http://stocktv.local");
  Serial.println("IP: " + ip);

  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");   // 台灣 UTC+8

  {
    std::unique_ptr<BearSSL::WiFiClientSecure> probe(new BearSSL::WiFiClientSecure);
    probe->setInsecure();
    useMfln = probe->probeMaxFragmentLength(TWSE_HOST, 443, 1024);
    Serial.printf("MFLN 1024: %d\n", useMfln);
  }

  MDNS.begin("stocktv");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifireset", HTTP_GET, handleWifiReset);
  server.begin();

  delay(2500);
  applyConfig();
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (pendingApply) {
    pendingApply = false;
    applyConfig();
  }

  uint32_t now = millis();
  uint32_t interval = marketOpen() ? (uint32_t)cfg.refreshSec * 1000UL : 600000UL;
  if (nStocks > 0 && (needRefresh || now - lastRefresh >= interval)) {
    needRefresh = false;
    doRefresh();
    lastRefresh = millis();
    drawPage();
  }

  if (pages > 1 && millis() - lastPage >= (uint32_t)cfg.pageSec * 1000UL) {
    curPage = (curPage + 1) % pages;
    lastPage = millis();
    drawPage();
  }

  if (timeOk()) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_min != lastMinute && nStocks > 0) drawHeader();
  }
}
