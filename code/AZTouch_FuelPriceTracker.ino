/*
  AZ-Touch 2.8" Fuel Price Tracker v1.0
  --------------------------------------
  Demonstrates:
    - ILI9341 graphics
    - XPT2046 touch input
    - REST/JSON data retrieval via WiFi
    - persistent touch settings (Preferences/NVS)
    - acoustic price alarm on GPIO 21
    - offline demo mode with simulated fuel prices

  Hardware target:
    AZ-Touch 2.8" + ESP32, current display version (yellow header)

  Fuel-price data: Tankerkoenig / MTS-K, CC BY 4.0
  https://creativecommons.tankerkoenig.de/

  NOTE: This is deliberately a compact example project, not a full product.
*/

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <XPT2046_Touchscreen.h>
#include "config.h"
#include "station_logos.h"

// --- AZ-Touch 2.8" hardware ---
#define TFT_DC      4
#define TFT_CS      5
#define TFT_LED    15
#define TFT_RST    22
#define TOUCH_CS   14
#define TOUCH_IRQ  27
#define BUZZER_PIN 21

#define TS_MINX   370
#define TS_MINY   470
#define TS_MAXX  3700
#define TS_MAXY  3600

static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;
static const unsigned long BASE_REFRESH_MS = 5UL * 60UL * 1000UL;
static const unsigned long PAGE_SWITCH_MS = 4UL * 1000UL;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

struct Station {
  String name, brand, place;
  float distance = 0, price = 0;
  bool valid = false;
};

enum FuelType : uint8_t { FUEL_DIESEL=0, FUEL_E10=1, FUEL_E5=2, FUEL_COUNT=3 };
Station stations[FUEL_COUNT][3];
FuelType displayFuel = FUEL_DIESEL;
uint8_t radiusKm = 10;
bool alarmEnabled = true;
float alarmPrice[FUEL_COUNT] = {1.50f, 1.60f, 1.65f};
bool alarmLatched[FUEL_COUNT] = {false,false,false};
bool alarmArmed = false;
bool settingsScreen = false;
unsigned long nextAutoFetch = 0, lastPageSwitch = 0;
uint32_t demoStep = 0;

const char* fuelApiName(FuelType f){ return f==FUEL_DIESEL?"diesel":(f==FUEL_E10?"e10":"e5"); }
const char* fuelDisplayName(FuelType f){ return f==FUEL_DIESEL?"Diesel":(f==FUEL_E10?"E10":"E5"); }

void saveSettings(){
  prefs.begin("fueltracker",false);
  prefs.putUChar("radius",radiusKm); prefs.putBool("alarm",alarmEnabled);
  prefs.putFloat("limD",alarmPrice[FUEL_DIESEL]); prefs.putFloat("lim10",alarmPrice[FUEL_E10]); prefs.putFloat("lim5",alarmPrice[FUEL_E5]);
  prefs.end();
}
void loadSettings(){
  prefs.begin("fueltracker",true);
  radiusKm=prefs.getUChar("radius",10); alarmEnabled=prefs.getBool("alarm",true);
  alarmPrice[FUEL_DIESEL]=prefs.getFloat("limD",1.50f); alarmPrice[FUEL_E10]=prefs.getFloat("lim10",1.60f); alarmPrice[FUEL_E5]=prefs.getFloat("lim5",1.65f);
  prefs.end();
  if(radiusKm!=3&&radiusKm!=10&&radiusKm!=20) radiusKm=10;
  for(int f=0;f<FUEL_COUNT;f++) if(alarmPrice[f]<0.50f||alarmPrice[f]>3.50f) alarmPrice[f]=1.50f+0.05f*f;
}
void beep(uint16_t freq=2400,uint16_t dur=120){ tone(BUZZER_PIN,freq,dur);delay(dur+35);noTone(BUZZER_PIN); }
void priceAlarm(){ beep(2300,110);delay(80);beep(2700,110);delay(80);beep(3100,180); }
void scheduleNextFetch(){ nextAutoFetch=millis()+BASE_REFRESH_MS+random(5000,45000); }

bool connectWiFi(){
  if(WiFi.status()==WL_CONNECTED) return true;
  Serial.print("[WiFi] Connecting to SSID: "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  unsigned long st=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-st<18000UL){ delay(250); Serial.print('.'); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("[WiFi] Connected, IP: "); Serial.println(WiFi.localIP());
    Serial.print("[WiFi] RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    return true;
  }
  Serial.print("[WiFi] Connection failed, status="); Serial.println((int)WiFi.status());
  return false;
}
void clearFuel(FuelType f){ for(auto &s:stations[f]) s=Station(); }
void sortFuel(FuelType f){
  for(int i=0;i<2;i++) for(int j=i+1;j<3;j++) if(stations[f][j].valid&&(!stations[f][i].valid||stations[f][j].price<stations[f][i].price)){Station t=stations[f][i];stations[f][i]=stations[f][j];stations[f][j]=t;}
}
void checkAlarm(FuelType f){
  if(!alarmEnabled || !stations[f][0].valid) { alarmLatched[f]=false; return; }
  // On boot / after SAVE only synchronize the latch. Never beep merely because
  // the currently loaded price is already below the configured threshold.
  if(!alarmArmed){ alarmLatched[f]=(stations[f][0].price<=alarmPrice[f]); return; }
  if(!alarmLatched[f] && stations[f][0].price<=alarmPrice[f]){ priceAlarm(); alarmLatched[f]=true; }
  else if(alarmLatched[f] && stations[f][0].price>alarmPrice[f]+0.05f) alarmLatched[f]=false;
}

void generateDemoFuel(FuelType f){
  clearFuel(f);
  // Eight demo brands. Across the three fuel pages all eight brands are
  // guaranteed to appear during every demo-data cycle.
  static const char* names[]={"ARAL","Shell","HEM","Esso","JET","TotalEnergies","star","AVIA"};
  static const char* places[]={"Ziesar","Genthin","Burg","Brandenburg","Moeckern","Wusterwitz","Magdeburg","Belzig"};
  float base=f==FUEL_DIESEL?1.469f:(f==FUEL_E10?1.579f:1.639f);
  for(int i=0;i<3;i++){
    uint8_t k=(uint8_t)((demoStep*3UL+(uint8_t)f*3U+(uint8_t)i)%8U); int cents=random(-3,4);
    auto &s=stations[f][i];s.name=names[k];s.brand=names[k];s.place=places[k];
    // Demo distances always provide three stations inside the selected radius.
    static const float d3[3]={0.8f,1.7f,2.6f};
    static const float d10[3]={1.7f,3.8f,7.4f};
    static const float d20[3]={2.1f,6.8f,14.2f};
    const float *dd=(radiusKm==3)?d3:((radiusKm==20)?d20:d10);
    s.distance=dd[i];s.price=base+0.018f*i+cents/100.0f;s.valid=true;
  }
  if((demoStep%4)==3 && stations[f][0].valid) stations[f][0].price=max(0.50f,alarmPrice[f]-0.011f);
  sortFuel(f); checkAlarm(f);
}
bool fetchDemo(){ for(int f=0;f<FUEL_COUNT;f++) generateDemoFuel((FuelType)f); demoStep++; nextAutoFetch=millis()+30UL*1000UL; return true; }

bool insertLiveStation(FuelType f, const JsonObject &j, float price){
  if(price<=0.0f) return false;
  Station candidate;
  candidate.name=String((const char*)(j["name"]|"Tankstelle"));
  candidate.brand=String((const char*)(j["brand"]|""));
  candidate.place=String((const char*)(j["place"]|""));
  candidate.distance=j["dist"]|0.0f;
  candidate.price=price; candidate.valid=true;
  // Keep only the three cheapest stations for this fuel.
  for(int pos=0;pos<3;pos++){
    if(!stations[f][pos].valid || candidate.price<stations[f][pos].price){
      for(int k=2;k>pos;k--) stations[f][k]=stations[f][k-1];
      stations[f][pos]=candidate; return true;
    }
  }
  return false;
}

bool fetchLiveAll(){
  Station old[FUEL_COUNT][3];
  for(int f=0;f<FUEL_COUNT;f++) for(int i=0;i<3;i++) old[f][i]=stations[f][i];
  for(int f=0;f<FUEL_COUNT;f++) clearFuel((FuelType)f);

  // type=all returns diesel/e10/e5 in one request. Tankerkoenig asks automated
  // clients to update all fuels with one call and not more often than needed.
  String url="https://creativecommons.tankerkoenig.de/json/list.php?lat="+String(LOCATION_LAT,6)+"&lng="+String(LOCATION_LNG,6)+"&rad="+String(radiusKm)+"&type=all&apikey="+String(TANKERKOENIG_API_KEY);
  Serial.println("[API] GET list.php type=all");
  Serial.print("[API] lat="); Serial.print(LOCATION_LAT,6); Serial.print(" lng="); Serial.print(LOCATION_LNG,6); Serial.print(" radius="); Serial.print(radiusKm); Serial.println(" km");
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(12000);
  if(!http.begin(client,url)){
    Serial.println("[API] http.begin failed");
    for(int f=0;f<FUEL_COUNT;f++)for(int i=0;i<3;i++)stations[f][i]=old[f][i]; return false;
  }
  int code=http.GET(); Serial.print("[API] HTTP status: "); Serial.println(code);
  if(code!=HTTP_CODE_OK){
    http.end(); for(int f=0;f<FUEL_COUNT;f++)for(int i=0;i<3;i++)stations[f][i]=old[f][i]; return false;
  }
  // Read the complete response first. Parsing the HTTP stream directly can fail
  // with ArduinoJson InvalidInput depending on transfer encoding / stream state.
  String payload=http.getString();
  http.end();
  Serial.print("[API] Payload length: "); Serial.print(payload.length()); Serial.println(" bytes");

  if(payload.length()==0){
    Serial.println("[API] Empty response body");
    for(int f=0;f<FUEL_COUNT;f++)for(int i=0;i<3;i++)stations[f][i]=old[f][i]; return false;
  }

  DynamicJsonDocument doc(49152);
  DeserializationError err=deserializeJson(doc,payload);
  if(err){
    Serial.print("[API] JSON error: "); Serial.println(err.c_str());
    for(int f=0;f<FUEL_COUNT;f++)for(int i=0;i<3;i++)stations[f][i]=old[f][i]; return false;
  }
  if(!doc["ok"].as<bool>()){
    Serial.print("[API] API error: "); Serial.println((const char*)(doc["message"]|"unknown"));
    for(int f=0;f<FUEL_COUNT;f++)for(int i=0;i<3;i++)stations[f][i]=old[f][i]; return false;
  }

  int received=0, openCount=0;
  for(JsonObject j:doc["stations"].as<JsonArray>()){
    received++;
    if(!(j["isOpen"]|false)) continue;
    openCount++;
    if(!j["diesel"].isNull()) insertLiveStation(FUEL_DIESEL,j,j["diesel"].as<float>());
    if(!j["e10"].isNull())    insertLiveStation(FUEL_E10,j,j["e10"].as<float>());
    if(!j["e5"].isNull())     insertLiveStation(FUEL_E5,j,j["e5"].as<float>());
  }
  Serial.print("[API] Stations received/open: "); Serial.print(received); Serial.print('/'); Serial.println(openCount);
  bool any=false;
  for(int f=0;f<FUEL_COUNT;f++){
    int n=0; for(int i=0;i<3;i++) if(stations[f][i].valid) n++;
    Serial.print("[API] "); Serial.print(fuelDisplayName((FuelType)f)); Serial.print(": "); Serial.print(n); Serial.println(" valid top stations");
    if(n){ any=true; checkAlarm((FuelType)f); }
    else for(int i=0;i<3;i++) stations[f][i]=old[f][i];
  }
  return any;
}

bool fetchStations(){
#if DEMO_MODE
  Serial.println("[MODE] Demo data refresh");
  return fetchDemo();
#else
  Serial.println("[MODE] Live data refresh");
  if(!connectWiFi()) { Serial.println("[API] Skipped: WiFi not connected"); scheduleNextFetch(); return false; }
  bool ok=fetchLiveAll(); scheduleNextFetch(); return ok;
#endif
}
String upper(String s){s.toUpperCase();return s;}
enum BrandId:uint8_t{BRAND_ARAL,BRAND_SHELL,BRAND_HEM,BRAND_ESSO,BRAND_JET,BRAND_TOTAL,BRAND_STAR,BRAND_AVIA,BRAND_UNKNOWN};
uint8_t brandId(const Station&s){String h=upper(s.brand+" "+s.name);if(h.indexOf("ARAL")>=0)return BRAND_ARAL;if(h.indexOf("SHELL")>=0)return BRAND_SHELL;if(h.indexOf("HEM")>=0||h.indexOf("TAMOIL")>=0)return BRAND_HEM;if(h.indexOf("ESSO")>=0)return BRAND_ESSO;if(h.indexOf("JET")>=0)return BRAND_JET;if(h.indexOf("TOTAL")>=0)return BRAND_TOTAL;if(h.indexOf("STAR")>=0||h.indexOf("ORLEN")>=0)return BRAND_STAR;if(h.indexOf("AVIA")>=0)return BRAND_AVIA;return BRAND_UNKNOWN;}
const uint16_t* logoFor(const Station&s){switch(brandId(s)){case BRAND_ARAL:return logo_aral;case BRAND_SHELL:return logo_shell;case BRAND_HEM:return logo_hem;case BRAND_ESSO:return logo_esso;case BRAND_JET:return logo_jet;case BRAND_TOTAL:return logo_total;case BRAND_STAR:return logo_star;case BRAND_AVIA:return logo_avia;default:return logo_fallback;}}
String shortText(String s,uint8_t n){if(s.length()<=n)return s;return s.substring(0,n-1)+".";}String priceDE(float p){String s=String(p,2);s.replace('.',',');return s;}String distDE(float d){String s=String(d,1);s.replace('.',',');return s+"km";}
void setFontSmall(){ tft.setFont(&FreeSans9pt7b); }
void setFontMedium(){ tft.setFont(&FreeSansBold12pt7b); }
void setFontLarge(){ tft.setFont(&FreeSansBold18pt7b); }
void setFontPrice(){ tft.setFont(&FreeSansBold24pt7b); }
void resetFont(){ tft.setFont(NULL); tft.setTextSize(1); }

void printCentered(const String &txt,int16_t cx,int16_t baseline){
  int16_t x1,y1; uint16_t w,h; tft.getTextBounds(txt,0,baseline,&x1,&y1,&w,&h);
  tft.setCursor(cx-(int16_t)w/2,baseline); tft.print(txt);
}
void drawGear(int16_t cx,int16_t cy){
  uint16_t c=tft.color565(135,135,135);
  for(int i=0;i<8;i++){float a=i*PI/4.0f;int x=cx+round(cos(a)*8),y=cy+round(sin(a)*8);tft.fillRect(x-2,y-2,4,4,c);}
  tft.fillCircle(cx,cy,7,c); tft.fillCircle(cx,cy,3,ILI9341_BLACK);
}


void drawWiFiStatus(int16_t x,int16_t y){
  uint16_t c=tft.color565(145,145,145);
  if(WiFi.status()!=WL_CONNECTED){
    tft.drawLine(x,y,x+10,y+10,c); tft.drawLine(x+10,y,x,y+10,c); return;
  }
  int32_t r=WiFi.RSSI();
  uint8_t bars=(r>=-55)?4:(r>=-67?3:(r>=-75?2:1));
  for(uint8_t i=0;i<4;i++){
    int h=3+i*3; int bx=x+i*4;
    if(i<bars) tft.fillRect(bx,y+12-h,3,h,c); else tft.drawRect(bx,y+12-h,3,h,c);
  }
}
void drawMainScreen(){
  tft.fillScreen(ILI9341_BLACK);
  // Header: fixed font sized for the longest title. 55 px zones remain at both sides.
  setFontMedium(); tft.setTextColor(ILI9341_WHITE);
  String title=String("Preise fuer ")+fuelDisplayName(displayFuel);
  printCentered(title,160,28);
#if DEMO_MODE
  resetFont(); tft.setTextSize(1); tft.setTextColor(tft.color565(145,145,145)); tft.setCursor(5,17); tft.print("Demo");
#else
  drawWiFiStatus(6,12);
#endif
  drawGear(302,21);
  tft.drawFastHLine(6,42,308,tft.color565(105,105,105));

  for(int i=0;i<3;i++){
    const int y=45+i*65;
    if(i) tft.drawFastHLine(6,y-2,308,tft.color565(85,85,85));
    Station &st=stations[displayFuel][i];
    if(!st.valid){ setFontLarge();tft.setTextColor(tft.color565(100,100,100));printCentered("--",160,y+38);continue; }
    tft.drawRGBBitmap(8,y+7,logoFor(st),STATION_LOGO_W,STATION_LOGO_H);
    setFontPrice(); tft.setTextColor(ILI9341_WHITE); tft.setCursor(104,y+43); tft.print(priceDE(st.price));
    setFontSmall(); tft.setCursor(218,y+23); tft.print(distDE(st.distance));
    tft.setCursor(218,y+46); tft.print(st.place); // no wrap: display edge clips long place names
  }
  resetFont();
}

void drawButton(int x,int y,int w,int h,const String&label,bool selected=false){
  uint16_t bg=selected?tft.color565(0,105,155):tft.color565(48,48,48);
  tft.fillRoundRect(x,y,w,h,5,bg); tft.drawRoundRect(x,y,w,h,5,tft.color565(125,125,125));
  setFontSmall(); tft.setTextColor(ILI9341_WHITE,bg); int16_t x1,y1;uint16_t tw,th;tft.getTextBounds(label,0,0,&x1,&y1,&tw,&th);
  tft.setCursor(x+(w-tw)/2,y+(h+th)/2-2);tft.print(label); resetFont();
}
void drawLimitRow(int y,const char*name,FuelType f){
  setFontSmall();tft.setTextColor(ILI9341_WHITE);tft.setCursor(10,y+23);tft.print(name);resetFont();
  drawButton(105,y,38,32,"-");
  setFontMedium();tft.setTextColor(ILI9341_YELLOW);tft.setCursor(157,y+24);tft.print(priceDE(alarmPrice[f]));resetFont();
  drawButton(270,y,38,32,"+");
}
void updateLimitValue(int y,FuelType f){
  // Redraw only the numeric value area; avoids full-screen flicker on +/- touches.
  tft.fillRect(148,y+2,112,28,ILI9341_BLACK);
  setFontMedium();tft.setTextColor(ILI9341_YELLOW);tft.setCursor(157,y+24);tft.print(priceDE(alarmPrice[f]));resetFont();
}
void updateRadiusButtons(){
  drawButton(105,34,60,28,"3 km",radiusKm==3);drawButton(173,34,60,28,"10 km",radiusKm==10);drawButton(241,34,67,28,"20 km",radiusKm==20);
}
void updateAlarmButton(){ drawButton(105,68,60,28,alarmEnabled?"AN":"AUS",alarmEnabled); }
void drawSettingsScreen(){
  tft.fillScreen(ILI9341_BLACK);
  setFontMedium();tft.setTextColor(ILI9341_WHITE);tft.setCursor(9,24);tft.print("Einstellungen");resetFont();
  setFontSmall();tft.setTextColor(ILI9341_LIGHTGREY);tft.setCursor(10,52);tft.print("Suchradius");resetFont();
  drawButton(105,34,60,28,"3 km",radiusKm==3);drawButton(173,34,60,28,"10 km",radiusKm==10);drawButton(241,34,67,28,"20 km",radiusKm==20);
  setFontSmall();tft.setTextColor(ILI9341_LIGHTGREY);tft.setCursor(10,87);tft.print("Preisalarm");resetFont();
  drawButton(105,68,60,28,alarmEnabled?"AN":"AUS",alarmEnabled);
  drawLimitRow(105,"Diesel",FUEL_DIESEL);drawLimitRow(142,"E10",FUEL_E10);drawLimitRow(179,"E5",FUEL_E5);
  drawButton(10,211,88,26,"BACK");drawButton(199,211,109,26,"SPEICHERN");
}
void drawCurrentScreen(){if(settingsScreen)drawSettingsScreen();else drawMainScreen();}

bool touchXY(int16_t&x,int16_t&y){
  if(!ts.touched())return false; TS_Point p=ts.getPoint();
  int px=map(p.x,TS_MINX,TS_MAXX,0,319);
  int py=map(p.y,TS_MINY,TS_MAXY,239,0);
  x=319-constrain(px,0,319);
  // Vertical axis correction for current yellow-header AZ-Touch MOD.
  y=constrain(py,0,239);
  delay(35);return true;
}
bool hit(int x,int y,int bx,int by,int bw,int bh){return x>=bx&&x<bx+bw&&y>=by&&y<by+bh;}
void handleMainTouch(int x,int y){if(hit(x,y,284,3,36,36)){settingsScreen=true;drawCurrentScreen();}}
void adjustLimit(FuelType f,float d,int y){alarmPrice[f]=constrain(alarmPrice[f]+d,0.50f,3.50f);updateLimitValue(y,f);}
void handleSettingsTouch(int x,int y){
  if(hit(x,y,10,211,88,29)){settingsScreen=false;lastPageSwitch=millis();drawCurrentScreen();return;}
  if(y>=34&&y<62){radiusKm=x<169?3:(x<237?10:20);updateRadiusButtons();return;}
  if(hit(x,y,105,68,60,28)){alarmEnabled=!alarmEnabled;if(!alarmEnabled)for(int f=0;f<FUEL_COUNT;f++)alarmLatched[f]=false;updateAlarmButton();return;}
  const int ys[3]={105,142,179};const FuelType fs[3]={FUEL_DIESEL,FUEL_E10,FUEL_E5};
  for(int i=0;i<3;i++)if(y>=ys[i]&&y<ys[i]+32){if(x>=105&&x<143)adjustLimit(fs[i],-0.05f,ys[i]);else if(x>=270&&x<308)adjustLimit(fs[i],0.05f,ys[i]);return;}
  if(hit(x,y,199,211,109,29)){
    saveSettings();
    // Do not destroy the currently displayed station data when leaving settings.
    // The next refresh updates the radius/data in the background.
    alarmArmed=false;
    for(int f=0;f<FUEL_COUNT;f++) checkAlarm((FuelType)f);
    alarmArmed=true;
    settingsScreen=false;
    nextAutoFetch=millis()+750UL;
    lastPageSwitch=millis();drawCurrentScreen();return;
  }
}

void setup(){Serial.begin(115200);delay(300);Serial.println();Serial.println("=== AZ-Touch Fuel Price Tracker v1.0 ===");
#if DEMO_MODE
Serial.println("[MODE] DEMO_MODE=true");
#else
Serial.println("[MODE] DEMO_MODE=false");
#endif
randomSeed((uint32_t)esp_random());pinMode(TFT_LED,OUTPUT);digitalWrite(TFT_LED,LOW);pinMode(BUZZER_PIN,OUTPUT);digitalWrite(BUZZER_PIN,LOW);SPI.begin();tft.begin();tft.setRotation(1);tft.setTextWrap(false);tft.fillScreen(ILI9341_BLACK);ts.begin();loadSettings();drawCurrentScreen();
#if !DEMO_MODE
connectWiFi();
#endif
alarmArmed=false;fetchStations();alarmArmed=true;lastPageSwitch=millis();drawCurrentScreen();}
void loop(){
  int16_t x,y;if(touchXY(x,y)){if(settingsScreen)handleSettingsTouch(x,y);else handleMainTouch(x,y);while(ts.touched())delay(10);}
  unsigned long now=millis();
  if(!settingsScreen && now-lastPageSwitch>=PAGE_SWITCH_MS){displayFuel=(FuelType)(((uint8_t)displayFuel+1)%FUEL_COUNT);lastPageSwitch=now;drawMainScreen();}
  if(!settingsScreen&&nextAutoFetch!=0&&(long)(now-nextAutoFetch)>=0){fetchStations();drawMainScreen();}
  delay(10);
}
