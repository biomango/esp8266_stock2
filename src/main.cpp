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
#define NAME_W 100            // 股名小圖尺寸 (由網頁設定頁的瀏覽器畫好後上傳)
#define NAME_H 22
char codes[MAX_STOCKS][16];   // 台股 "2330"；美股 "usAAPL"；加密貨幣 "ccBTCUSDT"
char names[MAX_STOCKS][48];   // 證交所回傳的中文股名 (UTF-8)，只用來讓網頁畫圖
Quote quotes[MAX_STOCKS];
bool needRedraw = false;
bool twDone = false;      // 台股收盤後只需抓一次
bool hasNonTw = false;    // 有美股/加密貨幣時，收盤也照常更新

inline bool isUs(const char *c) { return c[0] == 'u' && c[1] == 's'; }
inline bool isCc(const char *c) { return c[0] == 'c' && c[1] == 'c'; }
inline bool isTw(const char *c) { return !isUs(c) && !isCc(c); }
static const char *CRYPTO_QUOTES[] = {"FDUSD", "USDT", "USDC"};

// 螢幕上顯示的代號：美股去掉 us、加密貨幣去掉 cc 與計價幣 (ccBTCUSDT -> BTC)
void dispCode(const char *code, char *out, size_t n) {
  if (isUs(code)) { strlcpy(out, code + 2, n); return; }
  if (isCc(code)) {
    strlcpy(out, code + 2, n);
    size_t l = strlen(out);
    for (auto q : CRYPTO_QUOTES) {
      size_t ql = strlen(q);
      if (l > ql && strcmp(out + l - ql, q) == 0) { out[l - ql] = 0; break; }
    }
    return;
  }
  strlcpy(out, code, n);
}
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

// 代號正規化：台股 2330 / 00631L / T00(加權指數)；美股 usAAPL；加密貨幣 ccBTC (自動補 USDT)
String normalizeToken(String t) {
  t.toUpperCase();
  if (t.length() > 2 && t.startsWith("TW")) t = t.substring(2);   // 相容 tw2330 寫法
  if (t.length() > 2 && t.startsWith("US")) {
    String b = t.substring(2);
    return b.length() <= 8 ? "us" + b : String("");
  }
  if (t.length() > 2 && t.startsWith("CC")) {
    String b = t.substring(2);
    for (size_t i = 0; i < b.length(); i++) if (!isalnum((unsigned char)b[i])) return "";
    bool hasQuote = false;
    for (auto q : CRYPTO_QUOTES) if (b.length() > strlen(q) && b.endsWith(q)) hasQuote = true;
    if (!hasQuote) b += "USDT";
    return b.length() <= 12 ? "cc" + b : String("");
  }
  for (size_t i = 0; i < t.length(); i++) if (!isalnum((unsigned char)t[i])) return "";
  if ((t.length() >= 4 && t.length() <= 6) || t == "T00") return t;
  return "";
}

String sanitizeCodes(const String &in) {
  String out, tok;
  int n = 0;
  String src = in + "\n";
  for (size_t i = 0; i < src.length(); i++) {
    char c = src[i];
    if (isalnum((unsigned char)c) || c == '.' || c == '-') {
      tok += c;
    } else {
      if (tok.length() && n < MAX_STOCKS) {
        String nt = normalizeToken(tok);
        if (nt.length()) {
          if (out.length()) out += ',';
          out += nt;
          n++;
        }
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

void fmtPrice(float p, char *buf, size_t n, bool crypto = false) {
  if (p >= 10000) snprintf(buf, n, "%.0f", p);
  else if (p >= 1000) snprintf(buf, n, "%.1f", p);
  else if (crypto && p < 1) snprintf(buf, n, p >= 0.01f ? "%.4f" : "%.6f", p);
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

  // 狀態圓點：綠 = 開盤中，灰 = 收盤，紅 = 抓取失敗
  uint16_t sc = !lastOk ? TFT_RED : (marketOpen() ? TFT_GREEN : TFT_DARKGREY);
  tft.fillCircle(52, HEADER_H / 2, 4, sc);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, bg);
  tft.drawString(WiFi.localIP().toString(), 64, 2, 2);   // 本機 IP

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, bg);
  snprintf(buf, sizeof(buf), "%d/%d", curPage + 1, pages);
  tft.drawString(buf, 236, 2, 2);
  tft.setTextDatum(TL_DATUM);
}

// 股名小圖：每像素 1 byte 亮度 (0~255)，畫成灰色 (184,184,184) 與 stock1 相同
bool nameFileOk(const char *code) {
  File f = LittleFS.open(String("/n/") + code + ".bin", "r");
  if (!f) return false;
  bool ok = f.size() == NAME_W * NAME_H;   // 尺寸不符 (舊版小圖) 視為沒有，網頁會重新上傳
  f.close();
  return ok;
}

bool drawNameStrip(int x, int y, const char *code) {
  if (!nameFileOk(code)) return false;
  File f = LittleFS.open(String("/n/") + code + ".bin", "r");
  if (!f) return false;
  uint8_t line[NAME_W];
  uint16_t px[NAME_W];
  for (int r = 0; r < NAME_H; r++) {
    if (f.read(line, NAME_W) != NAME_W) break;
    for (int c = 0; c < NAME_W; c++) {
      uint8_t v = (uint16_t)line[c] * 184 / 255;
      px[c] = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
    }
    tft.pushImage(x, y + r, NAME_W, 1, px);
  }
  f.close();
  return true;
}

void drawRow(int slot, int idx) {
  int y0 = HEADER_H + slot * ROW_H;
  tft.fillRect(0, y0, 240, ROW_H, TFT_BLACK);
  if (idx >= nStocks) return;
  Quote &q = quotes[idx];
  char dc[16];
  dispCode(codes[idx], dc, sizeof(dc));
  bool cc = isCc(codes[idx]);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(dc, 6, y0 + 4, 4);

  tft.setTextDatum(TR_DATUM);
  if (!q.valid) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("--", 234, y0 + 4, 4);
  } else {
    float chg = q.price - q.prev;
    float pct = q.prev > 0 ? chg / q.prev * 100.0f : 0;
    uint16_t col = colorFor(chg);
    char buf[32];
    fmtPrice(q.price, buf, sizeof(buf), cc);
    tft.setTextColor(col, TFT_BLACK);
    tft.drawString(buf, 234, y0 + 4, 4);

    // 第二行右側：昨收 + 漲跌幅。空間不夠時昨收縮小字型，再不夠就不顯示
    snprintf(buf, sizeof(buf), "%+.2f%%", pct);
    tft.drawString(buf, 234, y0 + 34, 2);
    int wp = tft.textWidth(buf, 2);
    char pv[16], yb[24];
    fmtPrice(q.prev, pv, sizeof(pv), cc);
    snprintf(yb, sizeof(yb), "Y %s", pv);
    int limit = 6 + NAME_W + 4;                    // 股名區右緣
    int font = 2;
    if (234 - wp - 6 - tft.textWidth(yb, 2) < limit) font = 1;
    if (234 - wp - 6 - tft.textWidth(yb, font) >= limit) {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(yb, 234 - wp - 6, y0 + (font == 2 ? 34 : 38), font);
    }
  }
  if (isTw(codes[idx])) {
    drawNameStrip(6, y0 + 31, codes[idx]);          // 台股：中文股名小圖
  } else if (names[idx][0]) {                        // 美股：AAPL.OQ；加密貨幣：BTC/USDT
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(184, 184, 184), TFT_BLACK);
    tft.drawString(names[idx], 6, y0 + 34, 2);
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

// 回傳 -1 = 失敗，否則為回應中的股票筆數
int fetchBatchOnce(const int *ids, int count) {
  String exch;
  for (int k = 0; k < count; k++) {
    const char *cd = codes[ids[k]];
    if (k) exch += "%7C";
    if (strcmp(cd, "T00") == 0) exch += "tse_t00.tw";   // 加權指數只有上市
    else exch += "tse_" + String(cd) + ".tw%7Cotc_" + String(cd) + ".tw";
  }
  String url = String("https://" TWSE_HOST "/stock/api/getStockInfo.jsp?ex_ch=") + exch + "&json=1&delay=0&_=" + String(millis());

  if (twCookie.length() == 0) warmTw();   // 先暖機，且不與下面的 TLS 連線同時存在以省記憶體

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  if (useMfln) client->setBufferSizes(1024, 1024);

  HTTPClient http;
  http.useHTTP10(true);          // 避免 chunked，才能直接串流解析 JSON
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(*client, url)) return -1;
  if (twCookie.length()) http.addHeader("Cookie", twCookie);
  http.addHeader("Referer", "https://" TWSE_HOST "/stock/index.jsp");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP %d, heap=%u\n", code, ESP.getFreeHeap());
    http.end();
    return -1;
  }

  JsonDocument filter;
  JsonObject f = filter["msgArray"].add<JsonObject>();
  f["c"] = true; f["n"] = true; f["z"] = true; f["y"] = true; f["b"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return -1;
  }

  for (JsonObject o : doc["msgArray"].as<JsonArray>()) {
    const char *c = o["c"];
    if (!c) continue;
    for (int k = 0; k < count; k++) {
      int i = ids[k];
      if (strcasecmp(codes[i], c) != 0) continue;
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
      const char *nm = o["n"];
      if (nm) strlcpy(names[i], nm, sizeof(names[i]));
    }
  }
  return doc["msgArray"].as<JsonArray>().size();
}

bool fetchBatch(const int *ids, int count) {
  int n = fetchBatchOnce(ids, count);
  if (n == 0) {                 // 空回應：session 可能過期，重新取 cookie 再試一次
    twCookie = "";
    n = fetchBatchOnce(ids, count);
  }
  return n >= 0;
}

// ---- 美股：騰訊行情 (HTTP，GBK 編碼，只解析 ASCII 欄位)
// 每行 v_usAAPL="200~蘋果~AAPL.OQ~現價~昨收~...";  欄位 [2]=代碼 [3]=現價 [4]=昨收
int fetchUs(const int *ids, int count) {
  String list;
  for (int k = 0; k < count; k++) { if (k) list += ","; list += codes[ids[k]]; }
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(6000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(client, "http://qt.gtimg.cn/q=" + list)) return -1;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("US HTTP %d\n", code);
    http.end();
    return -1;
  }
  WiFiClient &st = http.getStream();
  int found = 0;
  uint32_t t0 = millis();
  while ((st.connected() || st.available()) && millis() - t0 < 8000) {
    if (!st.available()) { delay(5); continue; }
    String line = st.readStringUntil('\n');
    if (!line.startsWith("v_")) continue;
    int eq = line.indexOf('=');
    if (eq < 3) continue;
    String sym = line.substring(2, eq);
    String fld[5];
    int fi = 0, from = eq + 2;                       // 跳過 ="
    for (int p = from; p <= (int)line.length() && fi < 5; p++) {
      if (p == (int)line.length() || line[p] == '~') { fld[fi++] = line.substring(from, p); from = p + 1; }
    }
    if (fi < 5) continue;
    float price = fld[3].toFloat(), prev = fld[4].toFloat();
    if (price <= 0) continue;
    for (int k = 0; k < count; k++) {
      int i = ids[k];
      if (!sym.equalsIgnoreCase(codes[i])) continue;
      quotes[i].price = price;
      quotes[i].prev = prev > 0 ? prev : price;
      quotes[i].valid = true;
      strlcpy(names[i], fld[2].c_str(), sizeof(names[i]));
      found++;
    }
  }
  http.end();
  return found;
}

// ---- 加密貨幣：Binance 公開 24h 行情 (漲跌為滾動 24 小時)
bool mflnFor(const char *host) {   // 該主機是否支援小緩衝區 (省記憶體)，結果快取
  static String hs[4];
  static int8_t res[4];
  static int n = 0;
  for (int i = 0; i < n; i++) if (hs[i] == host) return res[i] == 1;
  std::unique_ptr<BearSSL::WiFiClientSecure> p(new BearSSL::WiFiClientSecure);
  p->setInsecure();
  bool r = p->probeMaxFragmentLength(host, 443, 1024);
  if (n < 4) { hs[n] = host; res[n] = r ? 1 : 0; n++; }
  return r;
}

void applyCrypto(JsonObject o, const int *ids, int count) {
  const char *sym = o["symbol"];
  if (!sym) return;
  float price = atof(o["lastPrice"] | "0");
  float chg = atof(o["priceChange"] | "0");
  if (price <= 0) return;
  for (int k = 0; k < count; k++) {
    int i = ids[k];
    if (strcasecmp(codes[i] + 2, sym) != 0) continue;
    quotes[i].price = price;
    quotes[i].prev = (price - chg) > 0 ? price - chg : price;
    quotes[i].valid = true;
    char dc[16];
    dispCode(codes[i], dc, sizeof(dc));
    snprintf(names[i], sizeof(names[i]), "%s/%s", dc, codes[i] + 2 + strlen(dc));
  }
}

// 回傳 -1 = 連線/解析失敗；0 = 查無此代號 (HTTP 400)；>0 = 筆數
int fetchCryptoOnce(const char *host, const int *ids, int count) {
  String url = String("https://") + host + "/api/v3/ticker/24hr?";
  if (count == 1) {
    url += "symbol=" + String(codes[ids[0]] + 2);
  } else {
    url += "symbols=%5B";
    for (int k = 0; k < count; k++) {
      if (k) url += "%2C";
      url += "%22" + String(codes[ids[k]] + 2) + "%22";
    }
    url += "%5D";
  }
  bool small = mflnFor(host);
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  if (small) client->setBufferSizes(1024, 1024);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(*client, url)) return -1;
  int code = http.GET();
  if (code == 400) { http.end(); return 0; }
  if (code != 200) {
    Serial.printf("CC %s HTTP %d heap=%u\n", host, code, ESP.getFreeHeap());
    http.end();
    return -1;
  }
  JsonDocument filter;
  if (count == 1) {
    filter["symbol"] = true; filter["lastPrice"] = true; filter["priceChange"] = true;
  } else {
    filter[0]["symbol"] = true; filter[0]["lastPrice"] = true; filter[0]["priceChange"] = true;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("CC JSON error: %s\n", err.c_str());
    return -1;
  }
  if (doc.is<JsonArray>()) {
    int n = 0;
    for (JsonObject o : doc.as<JsonArray>()) { applyCrypto(o, ids, count); n++; }
    return n;
  }
  applyCrypto(doc.as<JsonObject>(), ids, count);
  return 1;
}

bool fetchCrypto(const int *ids, int count) {
  static const char *HOSTS[] = {"api.binance.com", "data-api.binance.vision"};
  for (auto h : HOSTS) {
    int r = fetchCryptoOnce(h, ids, count);
    if (count == 1 ? r >= 0 : r > 0) return true;
  }
  if (count > 1) {   // 批次失敗 (例如有一檔代號打錯)：逐檔重試，讓正確的仍能顯示
    bool any = false;
    for (int k = 0; k < count; k++) {
      for (auto h : HOSTS) {
        if (fetchCryptoOnce(h, ids + k, 1) >= 0) { any = true; break; }
      }
    }
    return any;
  }
  return false;
}

void pauseServing(uint32_t ms) {   // 請求之間暫停，期間仍處理網頁請求
  uint32_t t = millis();
  while (millis() - t < ms) {
    server.handleClient();
    delay(10);
  }
}

void doRefresh() {
  bool ok = true;
  int idx[MAX_STOCKS];
  int n = 0;

  for (int i = 0; i < nStocks; i++) if (isTw(codes[i])) idx[n++] = i;
  if (n && (marketOpen() || !twDone)) {              // 台股收盤後只抓一次
    bool twOk = true;
    for (int s = 0; s < n; s += BATCH) {
      if (!fetchBatch(idx + s, min(BATCH, n - s))) twOk = false;
      pauseServing(1200);                            // 間隔 1.2 秒避免被證交所限流
    }
    if (twOk) twDone = true; else ok = false;
  }

  n = 0;
  for (int i = 0; i < nStocks; i++) if (isUs(codes[i])) idx[n++] = i;
  if (n && fetchUs(idx, n) < 0) ok = false;

  n = 0;
  for (int i = 0; i < nStocks; i++) if (isCc(codes[i])) idx[n++] = i;
  if (n && !fetchCrypto(idx, n)) ok = false;

  lastOk = ok;
  everRefreshed = true;
  Serial.printf("refresh done ok=%d heap=%u\n", ok, ESP.getFreeHeap());
}

// ---------------- 套用設定 ----------------
void cleanNameFiles() {   // 刪除已不在清單內的股名圖
  String del;
  Dir d = LittleFS.openDir("/n");
  while (d.next()) {
    String fn = d.fileName();
    String c = fn.substring(0, fn.length() - 4);   // 去掉 .bin
    bool keep = false;
    for (int i = 0; i < nStocks; i++) if (c.equalsIgnoreCase(codes[i])) keep = true;
    if (!keep) del += fn + "|";
  }
  int from = 0;
  while (true) {
    int p = del.indexOf('|', from);
    if (p < 0) break;
    LittleFS.remove("/n/" + del.substring(from, p));
    from = p + 1;
  }
}

void applyConfig() {
  nStocks = 0;
  String s = cfg.codes + ",";
  int from = 0;
  while (true) {
    int p = s.indexOf(',', from);
    if (p < 0) break;
    String t = s.substring(from, p);
    from = p + 1;
    if (t.length() >= 3 && nStocks < MAX_STOCKS) {
      t.toCharArray(codes[nStocks], 16);
      quotes[nStocks] = Quote();
      names[nStocks][0] = 0;
      nStocks++;
    }
  }
  pages = max(1, (nStocks + PER_PAGE - 1) / PER_PAGE);
  curPage = 0;
  twDone = false;
  hasNonTw = false;
  for (int i = 0; i < nStocks; i++) if (!isTw(codes[i])) hasNonTw = true;
  cleanNameFiles();
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
  h += F("<label>自選股（最多 20 檔；逗號、空白或換行分隔）<br><small style=\"font-weight:normal\">台股直接輸入代號（2330、0050，上市上櫃自動判斷，T00 為加權指數）；美股加 us（usAAPL）；加密貨幣加 cc（ccBTC、ccETH）</small></label>"
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

  h += F("<h3>目前報價</h3><p id=ns style='color:#666'></p><table><tr><th>代號</th><th>名稱</th><th>現價</th><th>昨收</th></tr>");
  for (int i = 0; i < nStocks; i++) {
    h += "<tr><td>" + String(codes[i]) + "</td><td>" + String(names[i]) + "</td><td>";
    if (quotes[i].valid) h += String(quotes[i].price, 2) + "</td><td>" + String(quotes[i].prev, 2);
    else h += String(everRefreshed ? "查無資料" : "等待中") + "</td><td>";
    h += "</td></tr>";
  }
  h += F("</table><p><a href=/wifireset onclick=\"return confirm('清除 WiFi 設定並重開機？')\">重設 WiFi</a></p>");
  // 股名小圖：小電視沒有中文字型，所以由瀏覽器把股名畫成 156x16 灰階圖，再上傳存進小電視
  h += F(R"JS(<script>
const W=100,H=22,FONT='"Microsoft JhengHei","PingFang TC","Noto Sans TC",sans-serif';
function render(t){
  const cv=document.createElement('canvas');cv.width=W;cv.height=H;
  const g=cv.getContext('2d',{willReadFrequently:true});
  g.fillStyle='#000';g.fillRect(0,0,W,H);
  g.fillStyle='#fff';g.textBaseline='middle';
  let s=18;g.font=s+'px '+FONT;
  while(s>9&&g.measureText(t).width>W){s--;g.font=s+'px '+FONT;}
  g.fillText(t,0,H/2+1);
  const d=g.getImageData(0,0,W,H).data;let h='';
  for(let i=0;i<W*H;i++)h+=(d[i*4]>>4).toString(16);
  return h;
}
let tries=0;
async function sync(){
  const ns=document.getElementById('ns');
  try{
    const list=await (await fetch('/names')).json();
    const done=new Set();let pending=0;
    for(const it of list){
      if(it.has||done.has(it.c))continue;
      if(!it.n){pending++;continue;}
      done.add(it.c);
      await fetch('/name',{method:'POST',body:new URLSearchParams({code:it.c,d:render(it.n)})});
    }
    if(done.size){ns.textContent='股名已更新';setTimeout(()=>location.reload(),600);}
    else if(pending&&++tries<20){ns.textContent='股名同步中…';setTimeout(sync,4000);}
  }catch(e){ns.textContent='股名同步失敗：'+e;}
}
sync();
</script></body></html>)JS");
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

void handleNames() {   // 給網頁設定頁用：目前每檔的股名，以及是否已有小圖
  String j = "[";
  for (int i = 0; i < nStocks; i++) {
    if (i) j += ",";
    String n;
    for (const char *c = names[i]; *c; c++) if (*c != '"' && *c != '\\' && (uint8_t)*c >= 32) n += *c;
    bool has = !isTw(codes[i]) || nameFileOk(codes[i]);
    j += "{\"c\":\"" + String(codes[i]) + "\",\"n\":\"" + n + "\",\"has\":" + (has ? "true" : "false") + "}";
  }
  j += "]";
  server.send(200, "application/json; charset=utf-8", j);
}

void handleName() {    // 接收瀏覽器畫好的股名圖 (156*16 個十六進位字元，每字元 4 bit 亮度)
  String code = server.arg("code");
  String d = server.arg("d");
  bool ok = code.length() >= 3 && code.length() <= 6 && d.length() == NAME_W * NAME_H;
  for (size_t i = 0; ok && i < code.length(); i++) if (!isalnum((unsigned char)code[i])) ok = false;
  if (!ok) { server.send(400, "text/plain", "bad"); return; }
  File f = LittleFS.open("/n/" + code + ".bin", "w");
  if (!f) { server.send(500, "text/plain", "fs"); return; }
  uint8_t buf[64];
  size_t j = 0;
  for (size_t i = 0; i < d.length(); i++) {
    char ch = d[i];
    int v = (ch >= '0' && ch <= '9') ? ch - '0' : ((ch | 32) >= 'a' && (ch | 32) <= 'f') ? (ch | 32) - 'a' + 10 : 0;
    buf[j++] = v * 17;
    if (j == sizeof(buf)) { f.write(buf, j); j = 0; }
  }
  if (j) f.write(buf, j);
  f.close();
  needRedraw = true;
  server.send(200, "text/plain", "ok");
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
  LittleFS.mkdir("/n");
  loadConfig();

  // 連續快速斷電重開 3 次 (每次都在開機完成前拔掉電源) -> 強制進入 WiFi 設定模式
  bool forceSetup = false;
  {
    int cnt = 0;
    File bf = LittleFS.open("/boot.cnt", "r");
    if (bf) { cnt = bf.parseInt(); bf.close(); }
    cnt++;
    if (cnt >= 3) {
      forceSetup = true;
      LittleFS.remove("/boot.cnt");
    } else {
      File bw = LittleFS.open("/boot.cnt", "w");
      if (bw) { bw.print(cnt); bw.close(); }
    }
  }

  tft.init();
  tft.setRotation(cfg.rot);
  tft.fillScreen(TFT_BLACK);
  setBacklight(cfg.bright);
  showMsg("StockTV", "Connecting WiFi...");

  // 先直接連上次儲存的 WiFi；連不上 (或強制設定模式) 才開設定頁
  WiFi.mode(WIFI_STA);
  if (forceSetup) {
    WiFi.disconnect(false);   // 不要自動重連舊 WiFi，等使用者設定新的
    delay(100);
  } else {
    WiFi.begin();
    for (uint32_t t0 = millis(); WiFi.status() != WL_CONNECTED && millis() - t0 < 15000; ) delay(200);
  }
  if (forceSetup || WiFi.status() != WL_CONNECTED) {
    showMsg("Setup WiFi", "Join WiFi: StockTV-Setup", "Open 192.168.4.1");
    WiFiManager wm;
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("StockTV-Setup");
    uint32_t t0 = millis(), lastTry = millis();
    while (WiFi.status() != WL_CONNECTED) {
      wm.process();
      if (!wm.getConfigPortalActive()) wm.startConfigPortal("StockTV-Setup");
      // 有人連著設定熱點時不要重試舊 WiFi，否則切換頻道會把手機踢掉
      if (WiFi.softAPgetStationNum() > 0) lastTry = millis();
      if (!forceSetup && WiFi.SSID().length() && millis() - lastTry > 25000) {
        lastTry = millis();
        WiFi.begin();
      }
      if (forceSetup && millis() - t0 > 300000UL) ESP.restart();   // 5 分鐘沒設定就恢復正常開機
      delay(5);
    }
    wm.stopConfigPortal();
  }

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
  server.on("/names", HTTP_GET, handleNames);
  server.on("/name", HTTP_POST, handleName);
  server.begin();

  delay(2500);
  applyConfig();
}

void loop() {
  server.handleClient();
  MDNS.update();

  static bool bootCleared = false;
  if (!bootCleared && millis() > 8000) {   // 順利開機超過 8 秒：清除快速重開計數
    bootCleared = true;
    LittleFS.remove("/boot.cnt");
  }
  static uint32_t lastConn = 0;
  if (WiFi.status() == WL_CONNECTED) lastConn = millis();
  else if (millis() - lastConn > 180000UL) ESP.restart();   // 斷線超過 3 分鐘 (例如換了路由器)：重開並進入設定頁

  if (pendingApply) {
    pendingApply = false;
    applyConfig();
  }

  if (needRedraw) {
    needRedraw = false;
    drawPage();
  }

  uint32_t now = millis();
  uint32_t interval = (marketOpen() || hasNonTw) ? (uint32_t)cfg.refreshSec * 1000UL : 600000UL;
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
