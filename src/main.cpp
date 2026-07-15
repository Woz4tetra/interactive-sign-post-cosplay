// Tip-to-play cosplay sign.
//
// When the sign is tipped over past a threshold angle, it plays a random WAV
// file from the microSD card. Hardware:
//
//   QT Py ESP32-S3 (5426)
//   Audio BFF (5769)   -> MAX98357 I2S amp + microSD, flush on the back
//   Charger BFF (5397) -> LiPo charging, hand-wired (5V, GND, monitor -> TX)
//   ADXL343 (4097)     -> accelerometer on the STEMMA QT port (Wire1)
//
// The board also hosts its own WiFi access point ("funnysign") serving an
// Undertale-styled control page: upload / delete / enable / disable WAV files
// on the SD card, watch the battery level, and read the accelerometer live.
//
// See README.md for wiring and SD-card details.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL343.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <Adafruit_NeoPixel.h>
#include <vector>

// ---------------------------------------------------------------------------
// Pin map: QT Py ESP32-S3 + Audio BFF (5769) + Charger BFF (5397)
// ---------------------------------------------------------------------------
// Audio BFF I2S -> MAX98357
static const int PIN_I2S_BCLK = A3;  // bit clock
static const int PIN_I2S_LRC  = A2;  // word select / LR clock
static const int PIN_I2S_DIN  = A1;  // serial data
// Audio BFF microSD, on the default SPI bus (SCK / MOSI / MISO pads)
static const int PIN_SD_CS    = A0;
// Charger BFF battery-voltage divider, hand-wired to TX (not A2)
static const int PIN_VBAT     = TX;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const float    AUDIO_GAIN_DEFAULT = 1.0f; // 0.0 - 1.0, adjustable in UI
static const uint32_t BATT_PERIOD = 30000;  // ms between battery prints
static const uint32_t SAMPLE_MS   = 20;     // accelerometer poll period
static const uint8_t  LED_BRIGHTNESS = 40;    // onboard NeoPixel brightness (0-255)

// LiPo voltage range for the battery gauge (1S cell).
static const float BATT_EMPTY = 3.30f;
static const float BATT_FULL  = 4.20f;

// USB-power detection. The Charger BFF divider senses the 5V rail through a
// schottky diode, so with USB plugged in it reads ~4.6-4.7V (5V minus the
// drop) - higher than even a full 4.2V battery. Hysteresis avoids flicker near
// the boundary. Tune on the bench if your diode drop differs.
static const float USB_ON_V  = 4.50f;  // rail above this => USB present
static const float USB_OFF_V = 4.35f;  // rail below this => on battery

// Tilt thresholds default to these angles (degrees from upright). The live
// values are adjustable from the web UI and persisted to the SD card. Defaults
// match the original cos-dot values: cos(90) = 0.00 fire, cos(53) = 0.60 re-arm.
static const float TRIGGER_DEG_DEFAULT = 90.0f;  // fire once tipped past this
static const float REARM_DEG_DEFAULT   = 53.0f;  // re-arm once back within this

// WiFi access point the control page is served from.
static const char *AP_SSID = "funnysign";
static const char *AP_PASS = "abiisaweenus";

// Config file listing disabled (muted) sound files, one path per line.
static const char *DISABLED_PATH = "/disabled.txt";

// Config file holding adjustable settings (key=value lines) on the SD card.
static const char *CONFIG_PATH = "/config.txt";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Adafruit_ADXL343 accel = Adafruit_ADXL343(343, &Wire1);

// Onboard NeoPixel (single RGB pixel) used as a status light.
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

WebServer server(80);
File uploadFile;  // destination handle for an in-progress upload

AudioGeneratorWAV *wav  = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S    *out  = nullptr;

// A WAV on the card plus whether it is allowed to play.
struct SoundFile {
  String name;
  bool   enabled;
};
std::vector<SoundFile> sounds;
std::vector<String>    disabledNames;  // paths muted via the web UI

float    refX = 0, refY = 0, refZ = 1;  // upright gravity reference (unit vector)
bool     armed = true;
bool     usbPowered = false;  // true while running on USB power (sounds muted)
uint32_t lastBatt = 0;

// Live tilt thresholds in degrees (see defaults above), plus their cached
// cosines. The loop compares against the cosines since dot == cos(angle).
float tipTriggerDeg = TRIGGER_DEG_DEFAULT;
float tipRearmDeg   = REARM_DEG_DEFAULT;
float tipTriggerDot = 0.0f;
float tipRearmDot   = 0.6f;

// Playback volume (0.0 - 1.0). Adjustable from the web UI and persisted.
float audioGain = AUDIO_GAIN_DEFAULT;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Set the onboard NeoPixel to an RGB color. Brightness is scaled globally by
// LED_BRIGHTNESS, so pass full 0-255 components here.
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// Live tip readout: green when upright, fading through yellow to red as the
// sign leans toward the trigger threshold. dot is cos(angle from upright).
static void ledTipReadout(float dot) {
  float frac = (dot - tipTriggerDot) / (1.0f - tipTriggerDot);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  ledSet((uint8_t)(255 * (1.0f - frac)), (uint8_t)(255 * frac), 0);
}

// Read the current gravity direction as a unit vector. Returns false if the
// accelerometer read fails or the magnitude is implausibly small.
static bool readGravity(float &x, float &y, float &z) {
  sensors_event_t e;
  if (!accel.getEvent(&e)) return false;
  float mag = sqrtf(e.acceleration.x * e.acceleration.x +
                    e.acceleration.y * e.acceleration.y +
                    e.acceleration.z * e.acceleration.z);
  if (mag < 0.1f) return false;
  x = e.acceleration.x / mag;
  y = e.acceleration.y / mag;
  z = e.acceleration.z / mag;
  return true;
}

static bool isNameDisabled(const String &n) {
  for (auto &d : disabledNames) if (d == n) return true;
  return false;
}

// Load the muted-file list from the SD card. Missing file just means none.
static void loadDisabled() {
  disabledNames.clear();
  File f = SD.open(DISABLED_PATH, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) disabledNames.push_back(line);
  }
  f.close();
}

// Rewrite the muted-file list to the SD card.
static void saveDisabled() {
  File f = SD.open(DISABLED_PATH, FILE_WRITE);  // "w" truncates
  if (!f) {
    Serial.println("[cfg] cannot write disabled.txt");
    return;
  }
  for (auto &d : disabledNames) f.println(d);
  f.close();
}

// Recompute the cached cosines whenever the degree thresholds change.
static void applyThresholds() {
  tipTriggerDot = cosf(tipTriggerDeg * PI / 180.0f);
  tipRearmDot   = cosf(tipRearmDeg   * PI / 180.0f);
}

// Load adjustable settings from the SD card, falling back to defaults.
static void loadConfig() {
  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = line.substring(0, eq);      key.trim();
      String val = line.substring(eq + 1);     val.trim();
      if      (key == "trigger") tipTriggerDeg = val.toFloat();
      else if (key == "rearm")   tipRearmDeg   = val.toFloat();
      else if (key == "gain")    audioGain     = val.toFloat();
    }
    f.close();
  }
  applyThresholds();
  audioGain = constrain(audioGain, 0.0f, 1.0f);
  Serial.printf("[cfg] trigger=%.1f deg, rearm=%.1f deg, gain=%.2f\n",
                tipTriggerDeg, tipRearmDeg, audioGain);
}

// Rewrite the settings file on the SD card.
static void saveConfig() {
  File f = SD.open(CONFIG_PATH, FILE_WRITE);  // "w" truncates
  if (!f) {
    Serial.println("[cfg] cannot write config.txt");
    return;
  }
  f.printf("trigger=%.1f\n", tipTriggerDeg);
  f.printf("rearm=%.1f\n", tipRearmDeg);
  f.printf("gain=%.2f\n", audioGain);
  f.close();
}

// Append a directory entry to the sound list if it is an enabled-or-muted WAV.
static void addSoundEntry(File &f) {
  if (f.isDirectory()) return;
  String name = f.name();
  String lower = name;
  lower.toLowerCase();
  if (!lower.endsWith(".wav")) return;
  if (!name.startsWith("/")) name = "/" + name;
  SoundFile sf;
  sf.name = name;
  sf.enabled = !isNameDisabled(name);
  sounds.push_back(sf);
  Serial.printf("[sd] found %s (%s)\n", name.c_str(), sf.enabled ? "on" : "off");
}

// Enumerate all WAV files on the SD root. Blocking; run once at boot and again
// after web-driven changes (upload/delete) so the list is always complete.
static void scanSounds() {
  sounds.clear();
  File root = SD.open("/");
  if (!root) {
    Serial.println("[sd] cannot open root");
    return;
  }
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    addSoundEntry(f);
    f.close();
  }
  root.close();
  Serial.printf("[sd] %u sound file(s)\n", (unsigned)sounds.size());
}

static void stopSound() {
  if (wav)  { wav->stop(); delete wav;  wav = nullptr; }
  if (file) { delete file; file = nullptr; }
}

// Start playing a randomly chosen clip. The file is opened here, right before
// playback begins, so nothing touches the SD card mid-stream. Does nothing if
// no file is enabled.
static void startRandomSound() {
  std::vector<int> playable;
  for (size_t i = 0; i < sounds.size(); i++)
    if (sounds[i].enabled) playable.push_back((int)i);
  if (playable.empty()) {
    Serial.println("[play] no enabled sounds on card");
    return;
  }
  int idx = playable[(int)random((long)playable.size())];
  Serial.printf("[play] %s\n", sounds[idx].name.c_str());
  file = new AudioFileSourceSD(sounds[idx].name.c_str());
  wav  = new AudioGeneratorWAV();
  if (!wav->begin(file, out)) {
    Serial.println("[play] failed to start");
    stopSound();
  } else {
    ledSet(255, 0, 255); // magenta while a clip is playing
  }
}

static float batteryVolts() {
  // The Charger BFF divider halves the pack voltage, so double the reading.
  // Average a handful of samples to steady the ADC noise.
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VBAT);
  return (mv / 8 * 2) / 1000.0f;
}

static int batteryPercent(float v) {
  float pct = (v - BATT_EMPTY) / (BATT_FULL - BATT_EMPTY) * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return (int)(pct + 0.5f);
}

static void printBattery() {
  float v = batteryVolts();
  Serial.printf("[batt] %.2f V (%d%%)%s\n", v, batteryPercent(v),
                usbPowered ? " [USB]" : "");
}

// Decide whether we're on USB power from the rail voltage, with hysteresis.
// Falls through to "on battery" if the monitor is unwired (reads ~0), so a
// missing sense line never wrongly mutes the sign.
static void updatePowerSource() {
  float v = batteryVolts();
  if (!usbPowered && v > USB_ON_V)       usbPowered = true;
  else if (usbPowered && v < USB_OFF_V)  usbPowered = false;
}

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------

// Escape a string for embedding in JSON (quotes / backslashes / control chars).
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c >= 0x20) { o += c; }
  }
  return o;
}

// Undertale-styled single-page control panel. Talks to the /api/* endpoints.
static const char INDEX_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FUNNY SIGN</title>
<style>
  *{box-sizing:border-box}
  html,body{margin:0;padding:0}
  body{background:#000;color:#fff;font-family:'Courier New',Courier,monospace;
    font-weight:bold;letter-spacing:1px;padding:16px;min-height:100vh}
  h1{text-align:center;font-size:22px;margin:6px 0 18px;letter-spacing:3px}
  h1 .heart{color:#f00}
  .box{border:4px solid #fff;background:#000;padding:14px 16px;margin:0 auto 18px;max-width:560px}
  .box h2{margin:0 0 12px;font-size:15px;color:#ff0;letter-spacing:2px}
  .row{display:flex;align-items:center;margin:7px 0}
  .label{width:88px;color:#fff}
  .val{color:#ff0}
  .star{color:#fff;margin-right:6px}
  .bar{flex:1;height:16px;border:2px solid #fff;background:#111;position:relative}
  .bar .fill{height:100%;background:#ff0;width:0%;transition:width .4s}
  .batt-pct{margin-left:10px;color:#ff0;width:46px;text-align:right}
  .batt-v{margin-left:8px;color:#fff}
  .file{display:flex;align-items:center;margin:8px 0}
  .file .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .file.off .name{color:#888;text-decoration:line-through}
  button{font-family:inherit;font-weight:bold;letter-spacing:1px;background:#000;color:#fff;
    border:3px solid #fff;padding:4px 10px;margin-left:8px;cursor:pointer;text-transform:uppercase}
  button:hover{color:#ff0}
  button:hover::before{content:'\2764 ';color:#f00}
  button.on{color:#ff0;border-color:#ff0}
  button.del:hover{color:#f55;border-color:#f55}
  .upload{margin-bottom:14px;border-bottom:2px dashed #444;padding-bottom:12px}
  input[type=file]{color:#fff;font-family:inherit;margin-top:8px}
  input[type=range]{flex:1;accent-color:#ff0;height:16px;cursor:pointer}
  .hint{color:#888;font-size:12px;margin-top:8px}
  .overlay{position:fixed;inset:0;background:rgba(0,0,0,.85);display:flex;align-items:center;justify-content:center}
  .overlay.hidden{display:none}
  .overlay .box{max-width:420px;width:90%;margin:0}
  .choices{display:flex;justify-content:flex-end;margin-top:16px}
  .toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);border:3px solid #fff;
    background:#000;color:#ff0;padding:8px 16px;display:none}
</style>
</head>
<body>
  <h1><span class="heart">&#10084;</span> FUNNY SIGN <span class="heart">&#10084;</span></h1>

  <div class="box">
    <h2>* SENSORS</h2>
    <div class="row">
      <span class="label">BATTERY</span>
      <div class="bar"><div class="fill" id="battFill"></div></div>
      <span class="batt-pct" id="battPct">--%</span>
      <span class="batt-v" id="battV">--V</span>
    </div>
    <div class="row"><span class="label">POWER</span><span class="val" id="power">--</span></div>
    <div class="row"><span class="label">TILT</span><span class="val" id="tilt">--</span></div>
    <div class="row"><span class="label">ACCEL</span><span class="val" id="accel">--</span></div>
    <div class="row"><span class="label">STATUS</span><span class="val" id="armed">--</span></div>
  </div>

  <div class="box">
    <h2>* SETTINGS</h2>
    <div class="row">
      <span class="label">VOLUME</span>
      <input type="range" id="volRange" min="0" max="100" step="1">
      <span class="val" id="volVal" style="width:52px;text-align:right">--%</span>
    </div>
    <div class="row">
      <span class="label">FIRE AT</span>
      <input type="range" id="trigRange" min="10" max="170" step="1">
      <span class="val" id="trigVal" style="width:52px;text-align:right">--&deg;</span>
    </div>
    <div class="row">
      <span class="label">RE-ARM AT</span>
      <input type="range" id="rearmRange" min="0" max="170" step="1">
      <span class="val" id="rearmVal" style="width:52px;text-align:right">--&deg;</span>
    </div>
    <div class="hint">* Bigger FIRE AT = tip further before it plays. RE-ARM AT is how upright it must return before it can fire again.</div>
    <div class="row" style="justify-content:flex-end">
      <button id="saveBtn">SAVE</button>
    </div>
  </div>

  <div class="box">
    <h2>* SOUND FILES</h2>
    <div class="upload">
      <div class="row"><span class="star">*</span>ADD A NEW SOUND (.WAV)</div>
      <input type="file" id="fileInput" accept=".wav">
      <button id="uploadBtn">UPLOAD</button>
      <div class="hint" id="uploadHint"></div>
    </div>
    <div id="files"><div class="hint">* loading...</div></div>
  </div>

  <div class="overlay hidden" id="modal">
    <div class="box">
      <p id="modalMsg">* Really do that?</p>
      <div class="choices">
        <button id="yesBtn">YES</button>
        <button id="noBtn">NO</button>
      </div>
    </div>
  </div>

  <div class="toast" id="toast"></div>

<script>
  const $ = id => document.getElementById(id);
  let settingsLoaded=false;
  function toast(msg){const t=$('toast');t.textContent=msg;t.style.display='block';
    clearTimeout(t._t);t._t=setTimeout(()=>t.style.display='none',2200);}

  function confirmBox(msg){
    return new Promise(res=>{
      $('modalMsg').textContent=msg;
      $('modal').classList.remove('hidden');
      const done=v=>{$('modal').classList.add('hidden');
        $('yesBtn').onclick=null;$('noBtn').onclick=null;res(v);};
      $('yesBtn').onclick=()=>done(true);
      $('noBtn').onclick=()=>done(false);
    });
  }

  function renderFiles(files){
    const box=$('files');
    if(!files.length){box.innerHTML='<div class="hint">* no sounds on the card</div>';return;}
    box.innerHTML='';
    files.forEach(f=>{
      const short=f.name.replace(/^\//,'');
      const row=document.createElement('div');
      row.className='file'+(f.enabled?'':' off');
      const name=document.createElement('span');
      name.className='name';name.textContent='* '+short;
      const tog=document.createElement('button');
      tog.className=f.enabled?'on':'';tog.textContent=f.enabled?'ON':'OFF';
      tog.onclick=async()=>{await fetch('/api/toggle?name='+encodeURIComponent(f.name),{method:'POST'});
        toast((f.enabled?'Disabled ':'Enabled ')+short);refresh();};
      const del=document.createElement('button');
      del.className='del';del.textContent='DEL';
      del.onclick=async()=>{if(await confirmBox('* Really delete "'+short+'"? This cannot be undone.')){
        await fetch('/api/delete?name='+encodeURIComponent(f.name),{method:'POST'});
        toast('Deleted '+short);refresh();}};
      row.appendChild(name);row.appendChild(tog);row.appendChild(del);
      box.appendChild(row);
    });
  }

  async function refresh(){
    try{
      const r=await fetch('/api/status');const s=await r.json();
      $('battFill').style.width=s.battPct+'%';
      $('battPct').textContent=s.battPct+'%';
      $('battV').textContent=s.battV.toFixed(2)+'V';
      $('power').textContent=s.usb?'USB (sounds muted)':'BATTERY';
      $('tilt').textContent=s.tipAngle.toFixed(1)+'°  (dot '+s.tipDot.toFixed(2)+')';
      $('accel').textContent='x:'+s.accel.x.toFixed(2)+'  y:'+s.accel.y.toFixed(2)+'  z:'+s.accel.z.toFixed(2);
      $('armed').textContent=s.armed?'ARMED':'TRIGGERED';
      // Populate the sliders once so periodic refreshes don't fight the user.
      if(!settingsLoaded){
        const vp=Math.round(s.gain*100);
        $('volRange').value=vp;$('volVal').textContent=vp+'%';
        $('trigRange').value=s.triggerDeg;$('trigVal').textContent=s.triggerDeg.toFixed(0)+'°';
        $('rearmRange').value=s.rearmDeg;$('rearmVal').textContent=s.rearmDeg.toFixed(0)+'°';
        settingsLoaded=true;
      }
      renderFiles(s.files);
    }catch(e){/* a clip is probably playing; try again next tick */}
  }

  $('volRange').oninput=()=>$('volVal').textContent=$('volRange').value+'%';
  $('trigRange').oninput=()=>$('trigVal').textContent=$('trigRange').value+'°';
  $('rearmRange').oninput=()=>$('rearmVal').textContent=$('rearmRange').value+'°';
  $('saveBtn').onclick=async()=>{
    const g=($('volRange').value/100).toFixed(2),t=$('trigRange').value,r=$('rearmRange').value;
    await fetch('/api/settings?gain='+g+'&trigger='+t+'&rearm='+r,{method:'POST'});
    toast('Settings saved');
    settingsLoaded=false;refresh();  // re-pull in case values were clamped
  };

  $('uploadBtn').onclick=async()=>{
    const inp=$('fileInput');
    if(!inp.files.length){toast('Pick a .wav first');return;}
    const fd=new FormData();fd.append('f',inp.files[0]);
    $('uploadHint').textContent='* uploading '+inp.files[0].name+' ...';
    try{await fetch('/api/upload',{method:'POST',body:fd});
      $('uploadHint').textContent='* upload complete!';inp.value='';refresh();}
    catch(e){$('uploadHint').textContent='* upload failed';}
  };

  refresh();
  setInterval(refresh,2000);
</script>
</body>
</html>)PAGE";

static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// JSON snapshot of battery, accelerometer, tip state, and the file list.
static void handleStatus() {
  float x = 0, y = 0, z = 0;
  bool ok = readGravity(x, y, z);
  float dot = ok ? (x * refX + y * refY + z * refZ) : 0.0f;
  float ang = acosf(constrain(dot, -1.0f, 1.0f)) * 180.0f / PI;
  float v = batteryVolts();

  String json = "{";
  json += "\"battV\":" + String(v, 2) + ",";
  json += "\"battPct\":" + String(batteryPercent(v)) + ",";
  json += "\"usb\":" + String(usbPowered ? "true" : "false") + ",";
  json += "\"armed\":" + String(armed ? "true" : "false") + ",";
  json += "\"accel\":{\"x\":" + String(x, 2) + ",\"y\":" + String(y, 2) +
          ",\"z\":" + String(z, 2) + "},";
  json += "\"tipDot\":" + String(dot, 2) + ",";
  json += "\"tipAngle\":" + String(ang, 1) + ",";
  json += "\"triggerDeg\":" + String(tipTriggerDeg, 1) + ",";
  json += "\"rearmDeg\":" + String(tipRearmDeg, 1) + ",";
  json += "\"gain\":" + String(audioGain, 2) + ",";
  json += "\"files\":[";
  for (size_t i = 0; i < sounds.size(); i++) {
    if (i) json += ",";
    json += "{\"name\":\"" + jsonEscape(sounds[i].name) +
            "\",\"enabled\":" + (sounds[i].enabled ? "true" : "false") + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Streamed multipart upload: write the incoming file straight to the SD card.
static void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String fn = up.filename;
    if (!fn.startsWith("/")) fn = "/" + fn;
    Serial.printf("[upload] start %s\n", fn.c_str());
    if (SD.exists(fn)) SD.remove(fn);
    uploadFile = SD.open(fn, FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.printf("[upload] done %u bytes\n", up.totalSize);
    scanSounds();  // pick up the new file
  }
}

static void handleDelete() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "missing name"); return; }
  String name = server.arg("name");
  if (!name.startsWith("/")) name = "/" + name;
  bool ok = SD.remove(name);
  // Drop it from the muted list too, if present.
  for (auto it = disabledNames.begin(); it != disabledNames.end();)
    if (*it == name) it = disabledNames.erase(it); else ++it;
  saveDisabled();
  scanSounds();
  Serial.printf("[delete] %s -> %s\n", name.c_str(), ok ? "ok" : "failed");
  server.send(ok ? 200 : 500, "text/plain", ok ? "deleted" : "failed");
}

static void handleToggle() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "missing name"); return; }
  String name = server.arg("name");
  if (!name.startsWith("/")) name = "/" + name;
  bool nowDisabled = !isNameDisabled(name);
  if (nowDisabled) {
    disabledNames.push_back(name);
  } else {
    for (auto it = disabledNames.begin(); it != disabledNames.end();)
      if (*it == name) it = disabledNames.erase(it); else ++it;
  }
  saveDisabled();
  for (auto &s : sounds) if (s.name == name) s.enabled = !nowDisabled;
  Serial.printf("[toggle] %s -> %s\n", name.c_str(), nowDisabled ? "disabled" : "enabled");
  server.send(200, "text/plain", nowDisabled ? "disabled" : "enabled");
}

// Update tilt thresholds (degrees) from the web UI, then persist them.
static void handleSettings() {
  if (server.hasArg("trigger")) tipTriggerDeg = server.arg("trigger").toFloat();
  if (server.hasArg("rearm"))   tipRearmDeg   = server.arg("rearm").toFloat();
  if (server.hasArg("gain"))    audioGain     = server.arg("gain").toFloat();
  // Clamp to a sane range and keep hysteresis: re-arm must be nearer upright
  // (a smaller angle) than the fire threshold, or the sign could lock up.
  tipTriggerDeg = constrain(tipTriggerDeg, 10.0f, 170.0f);
  tipRearmDeg   = constrain(tipRearmDeg,    0.0f, 170.0f);
  if (tipRearmDeg > tipTriggerDeg) tipRearmDeg = tipTriggerDeg;
  audioGain = constrain(audioGain, 0.0f, 1.0f);
  applyThresholds();
  if (out) out->SetGain(audioGain);  // takes effect immediately
  saveConfig();
  Serial.printf("[cfg] trigger=%.1f deg, rearm=%.1f deg, gain=%.2f\n",
                tipTriggerDeg, tipRearmDeg, audioGain);
  server.send(200, "text/plain", "saved");
}

static void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[wifi] AP '%s' up at http://%s\n", AP_SSID, ip.toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/upload", HTTP_POST,
            []() { server.send(200, "text/plain", "OK"); }, handleUpload);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/api/toggle", HTTP_POST, handleToggle);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.begin();
  Serial.println("[web] server started");
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTip-to-play sign starting");

  // Onboard NeoPixel as a status light. The QT Py gates pixel power on a
  // dedicated pin that must be driven high before the pixel will light.
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  ledSet(0, 0, 0);

  analogReadResolution(12);
  applyThresholds();  // valid cosines even if the SD config never loads

  // Accelerometer on the STEMMA QT port (Wire1 on this board).
  Wire1.setPins(SDA1, SCL1);
  Wire1.begin();
  if (!accel.begin()) {
    Serial.println("[accel] ADXL343 not found - check the STEMMA QT cable");
  } else {
    accel.setRange(ADXL343_RANGE_4_G);
    Serial.println("[accel] ready");
  }

  // microSD over the default SPI bus.
  ledSet(0, 0, 255); // blue: reading the card
  SPI.begin(SCK, MISO, MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[sd] card init failed");
    ledSet(255, 0, 0); // red: no card / init failed
    delay(800);
  } else {
    loadDisabled();
    loadConfig();
    scanSounds();
    // Summary color: green if clips loaded, amber if the card is fine but empty.
    ledSet(sounds.empty() ? 255 : 0, sounds.empty() ? 120 : 255, 0);
    delay(500);
  }

  // I2S output to the MAX98357 amp on the Audio BFF.
  out = new AudioOutputI2S();
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  out->SetGain(audioGain);  // loaded from config above (default if none)

  // Capture the current orientation as "upright". Hold the sign upright at
  // boot; tip detection is measured relative to this reference, so the mounting
  // angle does not matter.
  float sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 20; i++) {
    float x, y, z;
    if (readGravity(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(20);
  }
  if (n > 0) { refX = sx / n; refY = sy / n; refZ = sz / n; }
  Serial.printf("[tip] upright reference = (%.2f, %.2f, %.2f)\n", refX, refY, refZ);

  // Bring up the WiFi AP and control page.
  setupWeb();

  randomSeed(esp_random());
}

void loop() {
  // If a clip is playing, keep feeding it and ignore everything else until it
  // ends. Web requests wait until the sign is idle again (clips are short).
  if (wav && wav->isRunning()) {
    if (!wav->loop()) stopSound();
    return;
  }

  server.handleClient();
  updatePowerSource();

  float x, y, z;
  if (readGravity(x, y, z)) {
    // Dot product of two unit vectors == cosine of the angle from upright.
    float dot = x * refX + y * refY + z * refZ;
    Serial.print("[tip] angle: ");
    Serial.println(dot);
    if (usbPowered) ledSet(40, 40, 40);  // dim white: on USB, playback muted
    else            ledTipReadout(dot);  // live orientation readout
    if (armed && dot < tipTriggerDot) {
      armed = false;
      // Suppress playback while on USB power (bench / charging).
      if (usbPowered) Serial.println("[play] suppressed (USB power)");
      else            startRandomSound();
    } else if (!armed && dot > tipRearmDot) {
      armed = true;
      Serial.println("[tip] re-armed");
    }
  }

  uint32_t now = millis();
  if (now - lastBatt > BATT_PERIOD) {
    lastBatt = now;
    printBattery();
  }

  delay(SAMPLE_MS);
}
