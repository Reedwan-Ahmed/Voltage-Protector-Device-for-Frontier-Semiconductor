/*
 * ============================================================================
 *  VOLTAGE PROTECTOR DEVICE  -  Arduino Uno (ATmega328P) firmware

 *  WHAT IT DOES
 *    Continuously measures the line voltage and the load current, and
 *    disconnects the protected appliance (via a relay) the instant an
 *    over-voltage, under-voltage or over-current fault is detected. The
 *    load is automatically reconnected once the line has been healthy for
 *    a configurable time (AUTO mode) or on user command (MANUAL mode).
 *
 *  TWO WAYS TO CHANGE SETTINGS  
 *    1. On the device    : 16x2 LCD + 4 push buttons (MENU/UP/DOWN/RESET)
 *    2. From a computer   : USB serial, simple text commands (type HELP)
 *
 *
 *  The hardware-independent decision logic lives in protection_logic.h so
 *  it can be unit-tested on a PC (see tests/test_logic.cpp).
 * ============================================================================
 */

#include <LiquidCrystal.h>
#include <EEPROM.h>
#include "protection_logic.h"

/* ----------------------------- PIN MAP ----------------------------------- */
/* Sensing (analog inputs) */
const uint8_t PIN_VSENSE = A0;   /* pot emulating line voltage              */
const uint8_t PIN_ISENSE = A1;   /* pot emulating load current              */

/* Outputs (A2..A5 used as plain digital pins) */
const uint8_t PIN_RELAY  = A2;   /* HIGH = coil energised = load CONNECTED  */
const uint8_t PIN_LED_OK  = A3;  /* green: load on / normal                 */
const uint8_t PIN_LED_FLT = A4;  /* red:   fault / load disconnected        */
const uint8_t PIN_BUZZER  = A5;  /* sounder during a fault                  */

/* Buttons (active LOW, internal pull-ups) */
const uint8_t PIN_BTN_MENU  = 2; /* enter menu / next item / confirm        */
const uint8_t PIN_BTN_UP    = 3; /* increase value                          */
const uint8_t PIN_BTN_DOWN  = 4; /* decrease value                          */
const uint8_t PIN_BTN_RESET = 5; /* clear fault / reconnect / cancel menu   */

/* 16x2 LCD in 4-bit mode: LiquidCrystal(RS, E, D4, D5, D6, D7) */
LiquidCrystal lcd(13, 12, 11, 10, 9, 8);

/* ----------------------------- TUNABLES ---------------------------------- */
#define TRIP_CONFIRM_SAMPLES  2    /* consecutive bad reads before a trip   */
#define DEBOUNCE_MS          25
#define REPEAT_DELAY_MS      500    /* hold-to-repeat kicks in after this    */
#define REPEAT_RATE_MS       150
#define LCD_REFRESH_MS       250
#define TELEMETRY_MS        1000

#define EE_MAGIC_ADDR    0
#define EE_MAGIC_VALUE   0x27       /* bump this if the layout ever changes  */
#define EE_SETTINGS_ADDR 1

/* ----------------------------- STATE ------------------------------------- */
Settings settings;                 /* live settings (protection uses these) */
Settings settingsBackup;           /* snapshot taken when entering the menu */

uint16_t gVolts = 0;               /* latest measured line voltage, V       */
uint16_t gAmps_dA = 0;             /* latest measured load current, deci-A  */

bool     loadConnected = true;     /* relay/appliance state                 */
uint8_t  latchedFaults = FAULT_NONE;   /* fault(s) that caused the last trip*/
uint8_t  faultSamples  = 0;        /* trip confirmation counter             */
unsigned long firstFaultMs = 0;    /* when the current fault burst began     */
unsigned long healthySince = 0;    /* when the line last became healthy      */
bool     monEnabled = true;        /* periodic serial telemetry on/off      */

/* UI mode */
enum UiMode { UI_RUN, UI_MENU };
UiMode uiMode = UI_RUN;
int8_t menuIndex = 0;
#define MENU_COUNT 6               /* OV, UV, OC, DELAY, MODE, SAVE&EXIT     */

unsigned long tLastLcd = 0, tLastTelemetry = 0;

/* Serial line buffer */
char line[48];
uint8_t lineLen = 0;

/* ----------------------------- BUTTONS ----------------------------------- */
struct Button {
  uint8_t pin;
  bool repeatable;
  bool pressed;
  bool lastRaw;
  unsigned long tChange;
  unsigned long tNextRepeat;
  bool event;
};
Button bMenu, bUp, bDown, bReset;

void btnInit(Button &b, uint8_t pin, bool repeatable) {
  b.pin = pin; b.repeatable = repeatable;
  b.pressed = false; b.lastRaw = false;
  b.tChange = 0; b.tNextRepeat = 0; b.event = false;
  pinMode(pin, INPUT_PULLUP);
}

void btnPoll(Button &b, unsigned long now) {
  bool raw = (digitalRead(b.pin) == LOW);      /* pressed = LOW */
  if (raw != b.lastRaw) { b.lastRaw = raw; b.tChange = now; }
  if (now - b.tChange >= DEBOUNCE_MS) {
    if (raw != b.pressed) {
      b.pressed = raw;
      if (raw) { b.event = true; b.tNextRepeat = now + REPEAT_DELAY_MS; }
    } else if (raw && b.repeatable && (long)(now - b.tNextRepeat) >= 0) {
      b.event = true; b.tNextRepeat = now + REPEAT_RATE_MS;
    }
  }
}

bool btnConsume(Button &b) {
  if (b.event) { b.event = false; return true; }
  return false;
}

/* ----------------------------- EEPROM ------------------------------------ */
void clampSettings(Settings &s) {
  s.ovThreshold     = clampU16(s.ovThreshold,     OV_MIN,    OV_MAX);
  s.uvThreshold     = clampU16(s.uvThreshold,     UV_MIN,    UV_MAX);
  s.ocThreshold_dA  = clampU16(s.ocThreshold_dA,  OC_MIN_DA, OC_MAX_DA);
  s.reconnectDelayS = clampU16(s.reconnectDelayS, DELAY_MIN, DELAY_MAX);
  if (s.autoReconnect > 1) s.autoReconnect = 1;
}

void saveSettings() {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC_VALUE);
  EEPROM.put(EE_SETTINGS_ADDR, settings);
}

void loadSettings() {
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC_VALUE) {
    loadDefaults(settings);
    saveSettings();
  } else {
    EEPROM.get(EE_SETTINGS_ADDR, settings);
    clampSettings(settings);           /* guard against corrupted contents  */
  }
}

/* ----------------------------- HELPERS ----------------------------------- */
const char *faultName(uint8_t mask) {
  switch (mask) {
    case FAULT_NONE:        return "NONE";
    case FAULT_OVERVOLT:    return "OVER-VOLTAGE";
    case FAULT_UNDERVOLT:   return "UNDER-VOLTAGE";
    case FAULT_OVERCURRENT: return "OVER-CURRENT";
    default:                return "MULTIPLE";
  }
}
const char *faultShort(uint8_t mask) {
  switch (mask) {
    case FAULT_OVERVOLT:    return "OV!";
    case FAULT_UNDERVOLT:   return "UV!";
    case FAULT_OVERCURRENT: return "OC!";
    case FAULT_NONE:        return "";
    default:                return "MULTI";
  }
}

void printTimestamp() {
  unsigned long ms = millis();
  Serial.print('['); Serial.print(ms / 1000); Serial.print('.');
  Serial.print((ms % 1000) / 100); Serial.print("s] ");
}
void printAmps(uint16_t dA) { Serial.print(dA / 10); Serial.print('.'); Serial.print(dA % 10); }

void setIndicators() {
  digitalWrite(PIN_RELAY,   loadConnected ? HIGH : LOW);
  digitalWrite(PIN_LED_OK,  loadConnected ? HIGH : LOW);
  digitalWrite(PIN_LED_FLT, loadConnected ? LOW  : HIGH);
  digitalWrite(PIN_BUZZER,  loadConnected ? LOW  : HIGH);
}

/* ----------------------------- LCD RENDER -------------------------------- */
void lcdLine(uint8_t row, const char *text) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  for (; text[n] && n < 16; n++) lcd.write(text[n]);
  for (; n < 16; n++) lcd.write(' ');
}

void renderRun() {
  char buf[17];
  snprintf(buf, sizeof(buf), "V:%uV I:%u.%uA", gVolts, gAmps_dA / 10, gAmps_dA % 10);
  lcdLine(0, buf);
  if (loadConnected) {
    lcdLine(1, "LOAD:ON  NORMAL");
  } else {
    snprintf(buf, sizeof(buf), "LOAD:OFF %s", faultShort(latchedFaults));
    lcdLine(1, buf);
  }
}

void renderMenu() {
  char buf[17];
  switch (menuIndex) {
    case 0: lcdLine(0, "SET OVER-VOLT");
            snprintf(buf, sizeof(buf), "%u V        U/D", settings.ovThreshold); break;
    case 1: lcdLine(0, "SET UNDER-VOLT");
            snprintf(buf, sizeof(buf), "%u V        U/D", settings.uvThreshold); break;
    case 2: lcdLine(0, "SET OVER-CURR");
            snprintf(buf, sizeof(buf), "%u.%u A      U/D",
                     settings.ocThreshold_dA / 10, settings.ocThreshold_dA % 10); break;
    case 3: lcdLine(0, "RECONNECT DELAY");
            snprintf(buf, sizeof(buf), "%u s        U/D", settings.reconnectDelayS); break;
    case 4: lcdLine(0, "RECONNECT MODE");
            snprintf(buf, sizeof(buf), "%s      U/D",
                     settings.autoReconnect ? "AUTO  " : "MANUAL"); break;
    default: lcdLine(0, "SAVE & EXIT");
             snprintf(buf, sizeof(buf), "PRESS MENU"); break;
  }
  lcdLine(1, buf);
}

void render() { if (uiMode == UI_RUN) renderRun(); else renderMenu(); }

/* ----------------------------- CONNECT / TRIP ---------------------------- */
void disconnectLoad(uint8_t faults, unsigned long confirmMs) {
  loadConnected = false;
  latchedFaults = faults;
  healthySince  = 0;
  setIndicators();
  printTimestamp();
  Serial.print("!! TRIP: "); Serial.print(faultName(faults));
  Serial.print("  V="); Serial.print(gVolts); Serial.print("V I=");
  printAmps(gAmps_dA); Serial.print("A  -> LOAD DISCONNECTED (");
  Serial.print(confirmMs); Serial.println(" ms)");
  render();
}

void reconnectLoad(const char *why) {
  loadConnected = true;
  latchedFaults = FAULT_NONE;
  faultSamples  = 0;
  setIndicators();
  printTimestamp();
  Serial.print(">> RECONNECT ("); Serial.print(why);
  Serial.println(") -> LOAD ON");
  render();
}

/* ----------------------------- SERIAL ------------------------------------ */
void printHelp() {
  Serial.println(F("---- COMMANDS ----"));
  Serial.println(F("GET | STATUS         show readings + settings"));
  Serial.println(F("SET OV <volts>       over-voltage trip  (100-300)"));
  Serial.println(F("SET UV <volts>       under-voltage trip (50-290)"));
  Serial.println(F("SET OC <amps>        over-current trip  (0.5-20)"));
  Serial.println(F("SET DELAY <seconds>  auto-reconnect delay (1-600)"));
  Serial.println(F("SET MODE AUTO|MANUAL reconnect mode"));
  Serial.println(F("RESET                clear fault / reconnect now"));
  Serial.println(F("SAVE                 store settings in EEPROM"));
  Serial.println(F("MON ON | MON OFF     periodic telemetry on/off"));
  Serial.println(F("------------------"));
}

void printStatus() {
  Serial.println(F("----- STATUS -----"));
  Serial.print(F("Line Voltage : ")); Serial.print(gVolts); Serial.println(F(" V"));
  Serial.print(F("Load Current : ")); printAmps(gAmps_dA); Serial.println(F(" A"));
  Serial.print(F("Load Output  : "));
  if (loadConnected) Serial.println(F("ON (NORMAL)"));
  else { Serial.print(F("OFF (")); Serial.print(faultName(latchedFaults)); Serial.println(F(")")); }
  Serial.print(F("Mode / State : "));
  Serial.print(settings.autoReconnect ? F("AUTO") : F("MANUAL"));
  Serial.println(loadConnected ? F(" / running") : F(" / tripped"));
  Serial.println(F("-- Settings --"));
  Serial.print(F("Over-Voltage : ")); Serial.print(settings.ovThreshold); Serial.println(F(" V"));
  Serial.print(F("Under-Voltage: ")); Serial.print(settings.uvThreshold); Serial.println(F(" V"));
  Serial.print(F("Over-Current : ")); printAmps(settings.ocThreshold_dA); Serial.println(F(" A"));
  Serial.print(F("Reconnect    : ")); Serial.print(settings.autoReconnect ? F("AUTO after ") : F("MANUAL ("));
  if (settings.autoReconnect) { Serial.print(settings.reconnectDelayS); Serial.println(F(" s")); }
  else Serial.println(F("press RESET)"));
  Serial.println(F("------------------"));
}

void printStatusOneLineSettings();   /* forward declaration */

void processCommand(const char *cmd) {
  if (cmd[0] == '\0') return;
  CmdResult r = handleCommand(cmd, settings);
  switch (r) {
    case CMD_OK:
      Serial.print(F("OK. ")); printStatusOneLineSettings();
      Serial.println(F("(type SAVE to keep after power-off)"));
      render();
      break;
    case CMD_QUERY:   printStatus(); break;
    case CMD_HELP:    printHelp();   break;
    case CMD_SAVE:    saveSettings(); Serial.println(F("OK. Settings saved to EEPROM.")); break;
    case CMD_MON_ON:  monEnabled = true;  Serial.println(F("OK. Telemetry ON."));  break;
    case CMD_MON_OFF: monEnabled = false; Serial.println(F("OK. Telemetry OFF.")); break;
    case CMD_RESET:
      if (loadConnected) { Serial.println(F("Already connected.")); }
      else if (evaluateFaults(gVolts, gAmps_dA, settings) == FAULT_NONE) {
        reconnectLoad("manual");
      } else {
        printTimestamp();
        Serial.print(F("** Cannot reconnect: fault still present ("));
        Serial.print(faultName(evaluateFaults(gVolts, gAmps_dA, settings)));
        Serial.println(F(")"));
      }
      break;
    case CMD_BADARG:  Serial.println(F("ERR: bad/missing argument. Type HELP.")); break;
    default:          Serial.println(F("ERR: unknown command. Type HELP.")); break;
  }
}

void printStatusOneLineSettings() {
  Serial.print(F("OV=")); Serial.print(settings.ovThreshold);
  Serial.print(F(" UV=")); Serial.print(settings.uvThreshold);
  Serial.print(F(" OC=")); printAmps(settings.ocThreshold_dA);
  Serial.print(F(" DLY=")); Serial.print(settings.reconnectDelayS);
  Serial.print(F(" MODE=")); Serial.println(settings.autoReconnect ? F("AUTO") : F("MANUAL"));
}

void readSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { line[lineLen] = '\0'; processCommand(line); lineLen = 0; }
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}

/* ----------------------------- MENU INPUT -------------------------------- */
void adjust(int8_t dir) {              /* dir = +1 (UP) or -1 (DOWN) */
  switch (menuIndex) {
    case 0: settings.ovThreshold = clampU16((long)settings.ovThreshold + 5 * dir, OV_MIN, OV_MAX); break;
    case 1: settings.uvThreshold = clampU16((long)settings.uvThreshold + 5 * dir, UV_MIN, UV_MAX); break;
    case 2: settings.ocThreshold_dA = clampU16((long)settings.ocThreshold_dA + 5 * dir, OC_MIN_DA, OC_MAX_DA); break;
    case 3: settings.reconnectDelayS = clampU16((long)settings.reconnectDelayS + 1 * dir, DELAY_MIN, DELAY_MAX); break;
    case 4: settings.autoReconnect = settings.autoReconnect ? 0 : 1; break;
    default: break;
  }
}

void handleButtons(unsigned long now) {
  btnPoll(bMenu, now); btnPoll(bUp, now); btnPoll(bDown, now); btnPoll(bReset, now);

  if (uiMode == UI_RUN) {
    if (btnConsume(bMenu)) {                 /* enter menu                    */
      settingsBackup = settings;             /* so RESET can cancel edits     */
      uiMode = UI_MENU; menuIndex = 0; renderMenu();
    }
    if (btnConsume(bReset)) {                 /* clear fault / reconnect       */
      if (!loadConnected) {
        if (evaluateFaults(gVolts, gAmps_dA, settings) == FAULT_NONE) reconnectLoad("button");
        else { printTimestamp(); Serial.println(F("** RESET: fault still present")); }
      }
    }
    (void)btnConsume(bUp); (void)btnConsume(bDown);  /* ignored in RUN        */
  } else { /* UI_MENU */
    if (btnConsume(bMenu)) {
      menuIndex++;
      if (menuIndex >= MENU_COUNT) {          /* past SAVE&EXIT -> commit      */
        saveSettings();
        uiMode = UI_RUN;
        Serial.println(F("[device] settings saved from front panel"));
        render();
      } else renderMenu();
    }
    if (btnConsume(bReset)) {                 /* cancel: discard edits         */
      settings = settingsBackup;
      uiMode = UI_RUN;
      Serial.println(F("[device] menu cancelled (no changes saved)"));
      render();
    }
    if (btnConsume(bUp))   { adjust(+1); renderMenu(); }
    if (btnConsume(bDown)) { adjust(-1); renderMenu(); }
  }
}

/* ----------------------------- SETUP / LOOP ------------------------------ */
void setup() {
  Serial.begin(9600);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_FLT, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  btnInit(bMenu,  PIN_BTN_MENU,  false);
  btnInit(bUp,    PIN_BTN_UP,    true);
  btnInit(bDown,  PIN_BTN_DOWN,  true);
  btnInit(bReset, PIN_BTN_RESET, false);

  loadSettings();

  loadConnected = true;             /* start connected, protection will trip */
  latchedFaults = FAULT_NONE;
  setIndicators();

  lcd.begin(16, 2);
  lcd.clear();
  lcd.print("VoltageProtector");
  lcd.setCursor(0, 1);
  lcd.print("FSMB 2026  v1.0");

  Serial.println();
  Serial.println(F("=== Voltage Protector Device (Arduino Uno) ==="));
  Serial.println(F("Type HELP for commands."));
  printStatus();

  delay(1200);                      /* splash; only runs once, not in loop   */
  render();
  tLastLcd = tLastTelemetry = millis();
}

void loop() {
  unsigned long now = millis();

  /* ---- 1. FAST PATH: sense + protect (no blocking calls here) ---------- */
  gVolts   = adcToVolts(analogRead(PIN_VSENSE));
  gAmps_dA = adcToDeciAmps(analogRead(PIN_ISENSE));
  uint8_t active = evaluateFaults(gVolts, gAmps_dA, settings);

  if (loadConnected) {
    if (active != FAULT_NONE) {
      if (faultSamples == 0) firstFaultMs = now;
      if (++faultSamples >= TRIP_CONFIRM_SAMPLES) {
        disconnectLoad(active, now - firstFaultMs);
        faultSamples = 0;
      }
    } else {
      faultSamples = 0;
    }
  } else {
    if (active == FAULT_NONE) {
      if (healthySince == 0) healthySince = now;
      if (shouldAutoReconnect(active, settings, now - healthySince)) reconnectLoad("auto");
    } else {
      healthySince = 0;
    }
  }

  /* ---- 2. User input --------------------------------------------------- */
  handleButtons(now);
  readSerial();

  /* ---- 3. Throttled display + telemetry -------------------------------- */
  if (uiMode == UI_RUN && now - tLastLcd >= LCD_REFRESH_MS) {
    renderRun();
    tLastLcd = now;
  }
  if (monEnabled && now - tLastTelemetry >= TELEMETRY_MS) {
    printTimestamp();
    Serial.print("V="); Serial.print(gVolts); Serial.print("V I=");
    printAmps(gAmps_dA); Serial.print("A LOAD=");
    Serial.print(loadConnected ? "ON " : "OFF");
    Serial.print(" FAULT=");
    Serial.println(loadConnected ? "NONE" : faultName(latchedFaults));
    tLastTelemetry = now;
  }
}
