#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

/* ================== WiFi 配置 ================== */
const char* WIFI_SSID = "/";      // ← 改成你的 WiFi
const char* WIFI_PASS = "/";      // ← 改成你的密码

/* ================== 引脚映射（ESP-12F） ================== */
// 两个磁保持继电器（L9110S，互反脉冲）
const uint8_t R1_A = 16;
const uint8_t R1_B = 14;
const uint8_t R2_A = 12;
const uint8_t R2_B = 13;

// 两个按钮（到 GND，内部上拉）
const uint8_t BTN1 = 5;
const uint8_t BTN2 = 4;

// LED2：GPIO15 → 电阻 → LED → GND（高电平点亮）
const uint8_t LED2 = 15;

// “灯光”PWM 输出（默认复用 GPIO2 / LED1），低电平点亮
const uint8_t LIGHT_PIN        = 2;     // GPIO2
const bool    LIGHT_ACTIVE_LOW = true;  // 低电平点亮（GPIO2 常见接法）

/* ================== 参数 ================== */
const uint16_t PULSE_MS    = 30;   // 磁保持继电器脉冲宽度 20~50ms
const uint16_t DEBOUNCE_MS = 40;   // 按钮去抖

// 灯光参数（亮度/闪烁只在 relay1On=ON 时生效）
uint8_t  lightPct   = 50;     // 0~100 %
bool     blinkEn    = false;  // 闪烁开关
float    blinkHz    = 1.0f;   // 闪烁频率 Hz
uint32_t blinkNext  = 0;      // 下次相位切换时间
bool     blinkPhase = true;   // 当前相位

ESP8266WebServer server(80);
volatile bool relay1On = false, relay2On = false;

/* 最近动作（网页提示用） */
String   lastAct    = "BOOT";
uint32_t lastActSeq = 0;

/* 按钮去抖结构体 */
struct Btn {
  uint8_t pin;
  bool lastStable = true;   // INPUT_PULLUP：未按=HIGH
  bool lastRead   = true;
  uint32_t lastChg = 0;
} btn1{BTN1}, btn2{BTN2};

/* -------------- 继电器互反脉冲 -------------- */
void driveRelay(uint8_t A, uint8_t B, bool on) {
  if (on) {
    digitalWrite(A, HIGH);
    digitalWrite(B, LOW);
    delay(PULSE_MS);
    digitalWrite(A, LOW);
  } else {
    digitalWrite(A, LOW);
    digitalWrite(B, HIGH);
    delay(PULSE_MS);
    digitalWrite(B, LOW);
  }
}

/* -------------- 灯光 PWM -------------- */
inline uint16_t pctToPwm(uint8_t pct){
  uint16_t v = map(constrain(pct,0,100), 0,100, 0,1023);
  if (LIGHT_ACTIVE_LOW) v = 1023 - v; // 低电平点亮 → 反相
  return v;
}
void lightOutput(uint8_t pct){
  analogWrite(LIGHT_PIN, pctToPwm(pct));
}

/* “总控逻辑”：灯光仅在 relay1On==true 时输出，OFF 时强制 0% */
void applyLightByMode(){
  if (!relay1On) { lightOutput(0); return; }
  if (!blinkEn)  { lightOutput(lightPct); return; }
  // 闪烁：50% 占空比
  uint32_t now = millis();
  uint32_t halfPeriod = (uint32_t)((blinkHz > 0.01f) ? (500.0f / blinkHz) : 500);
  if(now >= blinkNext){
    blinkPhase = !blinkPhase;
    blinkNext = now + halfPeriod;
  }
  lightOutput(blinkPhase ? lightPct : 0);
}

/* -------------- LED2 跟继电器2（高=亮，低=灭） -------------- */
void updateLED2() {
  digitalWrite(LED2, relay2On ? HIGH : LOW);
}

/* -------------- 网页 UI -------------- */
String htmlPage() {
  String s = R"====(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 Relay + Light</title>
<style>
body{font-family:system-ui;margin:20px}
.card{max-width:620px;margin:auto;border:1px solid #ddd;border-radius:12px;padding:16px;box-shadow:0 2px 8px #0001}
.row{display:flex;gap:12px;margin:10px 0;align-items:center}
.btn{flex:1;padding:12px;border:0;border-radius:10px;background:#007aff;color:#fff;font-size:16px}
.btn.off{background:#ff3b30}
.small{color:#666;font-size:12px}
.tag{padding:2px 8px;border-radius:999px;background:#eee;margin-left:6px}
.on{background:#0a0;color:#fff;padding:2px 8px;border-radius:8px}
.off{background:#a00;color:#fff;padding:2px 8px;border-radius:8px}
label{min-width:76px}
input[type=range]{flex:1}
input[type=number]{width:90px;padding:6px 8px}
.switch{display:flex;align-items:center;gap:8px}
</style></head><body>
<div class="card">
  <h1>继电器与灯光控制 <span id="ip" class="tag"></span></h1>
  <div class="small">最近动作：<span id="act">-</span></div>
  <div id="st" class="small">加载中…</div>

  <h3>继电器</h3>
  <div class="row">
    <button class="btn" onclick="cmd('r1',1)">继电器1 ON</button>
    <button class="btn off" onclick="cmd('r1',0)">继电器1 OFF</button>
  </div>
  <div class="row">
    <button class="btn" onclick="cmd('r2',1)">继电器2 ON</button>
    <button class="btn off" onclick="cmd('r2',0)">继电器2 OFF</button>
  </div>
  <div class="row">
    <button class="btn" onclick="fetch('/api/toggle?id=1').then(u)">切换1</button>
    <button class="btn" onclick="fetch('/api/toggle?id=2').then(u)">切换2</button>
  </div>

  <h3>灯光（仅在“继电器1=ON”时生效）</h3>
  <div class="row">
    <label>亮度</label>
    <input id="sl" type="range" min="0" max="100" step="1" oninput="setLight(this.value)">
    <span id="slv" class="tag">0%</span>
  </div>
  <div class="row">
    <div class="switch">
      <input id="blink" type="checkbox" onchange="setBlink()">
      <label for="blink">闪烁</label>
    </div>
    <label>频率(Hz)</label>
    <input id="hz" type="number" min="0.1" max="20" step="0.1" value="1.0" onchange="setBlink()" oninput="setBlink()">
  </div>
</div>

<script>
let lastSeq=-1, lockUI=false;
function paint(j){
  st.innerHTML='继电器1: ' + (j.r1?'<span class="on">ON</span>':'<span class="off">OFF</span>')
             + '　继电器2: ' + (j.r2?'<span class="on">ON</span>':'<span class="off">OFF</span>')
             + '　灯光: ' + (j.r1 ? (j.blink?('<span class="on">闪</span>@'+j.hz.toFixed(1)+'Hz'):(j.lp+'%')) : '<span class="off">OFF</span>');
  if(j.seq!==lastSeq){
    lastSeq=j.seq;
    act.innerText=j.act+' @ '+new Date().toLocaleTimeString();
  }
  if(!lockUI){
    document.getElementById('sl').value = j.lp;
    document.getElementById('slv').innerText = j.lp + '%';
    document.getElementById('blink').checked = j.blink;
    document.getElementById('hz').value = j.hz.toFixed(1);
  }
}
function u(){fetch('/api/state').then(r=>r.json()).then(paint);}
function cmd(id,on){fetch('/api/'+id+'?on='+on).then(u);}

let sl = document.getElementById('sl');
sl.addEventListener('mousedown',()=>lockUI=true);
sl.addEventListener('touchstart',()=>lockUI=true);
sl.addEventListener('mouseup',()=>{lockUI=false;});
sl.addEventListener('touchend',()=>{lockUI=false;});
function setLight(v){
  document.getElementById('slv').innerText = v + '%';
  fetch('/api/light?pct='+v).then(u);
}
function setBlink(){
  let en = document.getElementById('blink').checked ? 1 : 0;
  let hz = parseFloat(document.getElementById('hz').value||"1.0");
  if(!isFinite(hz) || hz<=0) hz=1.0;
  fetch('/api/blink?ena='+en+'&hz='+hz).then(u);
}
window.onload=()=>{
  u(); fetch('/api/ip').then(r=>r.text()).then(t=>ip.innerText=t);
  setInterval(u, 500);
}
</script>
</body></html>
)====";
  return s;
}

/* -------------- Web API -------------- */
void handleState(){
  String j = String("{\"r1\":")+(relay1On?"true":"false")
           +",\"r2\":"+(relay2On?"true":"false")
           +",\"lp\":"+String(lightPct)
           +",\"blink\":"+(blinkEn?"true":"false")
           +",\"hz\":"+String(blinkHz,1)
           +",\"act\":\""+lastAct+"\""
           +",\"seq\":"+String(lastActSeq)+"}";
  server.send(200,"application/json",j);
}
void handleIp(){ server.send(200,"text/plain",WiFi.localIP().toString()); }

void setRelay(uint8_t id,bool on,const char* src){
  if(id==1 && relay1On!=on){
    relay1On=on;
    driveRelay(R1_A,R1_B,on);
    updateLED2();                 // 注意：这里只让 LED2 跟随继电器2 的状态，但调用没问题
    blinkNext = millis();         // 重置闪烁时间基
    lastAct = String(src)+" R1 "+(on?"ON":"OFF");
    lastActSeq++;
  }
  if(id==2 && relay2On!=on){
    relay2On=on;
    driveRelay(R2_A,R2_B,on);
    updateLED2();
    lastAct = String(src)+" R2 "+(on?"ON":"OFF");
    lastActSeq++;
  }
}
void handleToggle(){
  uint8_t id = server.hasArg("id") ? server.arg("id").toInt() : 0;
  if(id==1) setRelay(1,!relay1On,"WEB");
  if(id==2) setRelay(2,!relay2On,"WEB");
  handleState(); // ← 返回响应，防止前端等待导致重复触发
}
void handleLight(){
  if(server.hasArg("pct")){
    int v = constrain(server.arg("pct").toInt(),0,100);
    lightPct = (uint8_t)v;
    lastAct = String("WEB LIGHT ")+String(lightPct)+"%";
    lastActSeq++;
    // 若继电器1=ON且非闪烁，立即体现
    if(relay1On && !blinkEn) lightOutput(lightPct);
  }
  handleState();
}
void handleBlink(){
  if(server.hasArg("ena")) blinkEn = (server.arg("ena")=="1");
  if(server.hasArg("hz")){
    float hz = server.arg("hz").toFloat();
    if(!isnan(hz) && hz>0.05f && hz<50.0f) blinkHz = hz;
  }
  blinkPhase = true;         // 重置相位
  blinkNext  = millis();
  lastAct = String("WEB BLINK ")+(blinkEn?("ON @ "+String(blinkHz,1)+"Hz"):"OFF");
  lastActSeq++;
  handleState();
}

/* -------------- 按钮扫描 -------------- */
void scanBtn(Btn &b, bool &toggleFlag){
  bool v = digitalRead(b.pin);
  if(v!=b.lastRead){
    b.lastRead=v;
    b.lastChg=millis();
  }
  if(millis()-b.lastChg>DEBOUNCE_MS){
    if(v!=b.lastStable){
      b.lastStable=v;
      if(v==LOW) toggleFlag=true;
    }
  }
}

/* -------------- setup -------------- */
void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");

  // 继电器输出脚
  pinMode(R1_A,OUTPUT); pinMode(R1_B,OUTPUT);
  pinMode(R2_A,OUTPUT); pinMode(R2_B,OUTPUT);
  digitalWrite(R1_A,LOW); digitalWrite(R1_B,LOW);
  digitalWrite(R2_A,LOW); digitalWrite(R2_B,LOW);

  // 按钮上拉
  pinMode(BTN1,INPUT_PULLUP);
  pinMode(BTN2,INPUT_PULLUP);

  // LED2（高电平点亮；且 GPIO15 上电需为 LOW → 默认拉低更安全）
  pinMode(LED2,OUTPUT); 
  digitalWrite(LED2, LOW);   // 默认灭、且满足启动要求

  // 灯光 PWM 初始化（GPIO2）
  pinMode(LIGHT_PIN, OUTPUT);
  analogWriteRange(1023);
  analogWriteFreq(1000);      // 1kHz
  lightOutput(0);             // 上电默认灭（等继电器1=ON 后再输出）

  // 不再从 EEPROM 恢复，默认继电器全 OFF、LED2 随之 OFF
  relay1On = false;
  relay2On = false;
  updateLED2();

  // WiFi & mDNS
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  Serial.printf("Connecting to %s ...\n", WIFI_SSID);
  uint32_t t0 = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status()==WL_CONNECTED) {
    Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
    if(MDNS.begin("relay")) Serial.println("mDNS started: relay.local");
  } else {
    Serial.println("WiFi failed.");
  }

  // 路由
  server.on("/", [](){ server.send(200,"text/html; charset=utf-8",htmlPage()); });
  server.on("/api/state", handleState);
  server.on("/api/ip",    handleIp);
  server.on("/api/r1",    [](){
    bool on = server.hasArg("on") && server.arg("on")=="1";
    setRelay(1, on, "WEB");
    handleState();
  });
  server.on("/api/r2",    [](){
    bool on = server.hasArg("on") && server.arg("on")=="1";
    setRelay(2, on, "WEB");
    handleState();
  });
  server.on("/api/toggle",handleToggle);
  server.on("/api/light", handleLight);
  server.on("/api/blink", handleBlink);
  server.begin();

  Serial.println("HTTP server started");
}

/* -------------- loop -------------- */
void loop() {
  server.handleClient();
  MDNS.update();

  // 灯光输出遵循“继电器1总控”
  applyLightByMode();

  // 扫描按钮
  bool t1=false,t2=false;
  scanBtn(btn1,t1);
  scanBtn(btn2,t2);
  if(t1){ setRelay(1, !relay1On, "BTN1"); }
  if(t2){ setRelay(2, !relay2On, "BTN2"); }
}
