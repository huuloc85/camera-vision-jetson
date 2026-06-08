// ═══════════════════════════════════════════════════════════
// ESP32-S3 GPIO Bridge — Jetson ↔ ESP32-S3 ↔ Opto ↔ PLC
// Board: MKE-K01 ESP32-S3 Dev Kit + MKE-B01 IO Shield
// ═══════════════════════════════════════════════════════════
//
// Jetson → UART → ESP32-S3 → Opto (NPN) → PLC   [OUTPUT]
// PLC    → Opto → ESP32-S3 → UART → Jetson       [INPUT]
//
// Wiring:
//   Jetson TX (pin8)  → ESP32-S3 GPIO16 (UART1 RX)
//   Jetson RX (pin10) ← ESP32-S3 GPIO17 (UART1 TX)
//   Jetson GND        → ESP32-S3 GND
//
// Flow: PLC trigger accepted only while idle → clear OK/NG → BUSY=ON → UART "TRIGGER" → Jetson inspect
//       → "OK_ON"/"NG_ON" → OK/NG=ON (level hold) + BUSY=OFF
//       → PLC reads OK/NG level when BUSY=OFF → next trigger clears result

#include <esp_system.h>

// ── Pins (ESP32-S3 MKE-K01) ─────────────────────────────
// ⚠️ S3 restricted: GPIO19/20=USB, GPIO22-32=Flash/PSRAM
#define UART_RX       16    // ← Jetson TX (wire to ESP32 GPIO16 pin)
#define UART_TX       17    // → Jetson RX (wire to ESP32 GPIO17 pin)

#define PLC_TRIGGER    4    // Opto INPUT (riêng) ← PLC trigger

// Opto OUTPUT 4-ch module: GPIO liền nhau 5→8 = CH1→CH4
#define PIN_OK         5    // CH1 → PLC OK
#define PIN_NG         6    // CH2 → PLC NG
#define PIN_BUSY       7    // CH3 → PLC BUSY
#define PIN_LIGHT      8    // CH4 → Đèn chiếu sáng

// ── Opto/Relay drive logic ───────────────────────────────
// Set to 0 = active-HIGH push-pull (tested OK with test_uart.ino)
// Set to 1 = active-LOW open-drain (NPN opto)
#define OPTO_ACTIVE_LOW 0

#if OPTO_ACTIVE_LOW
  #define OPTO_ON(pin)   digitalWrite(pin, LOW)
  #define OPTO_OFF(pin)  digitalWrite(pin, HIGH)
  static const uint8_t OPTO_OFF_LEVEL = HIGH;
  static const uint8_t OPTO_PIN_MODE  = OUTPUT_OPEN_DRAIN;
#else
  #define OPTO_ON(pin)   digitalWrite(pin, HIGH)
  #define OPTO_OFF(pin)  digitalWrite(pin, LOW)
  static const uint8_t OPTO_OFF_LEVEL = LOW;
  static const uint8_t OPTO_PIN_MODE  = OUTPUT;
#endif

// ── Trigger edge (1=RISING/PNP, 0=FALLING/NPN) ──────────
#define PLC_TRIGGER_ACTIVE_HIGH 1

#if PLC_TRIGGER_ACTIVE_HIGH
  static const uint8_t PLC_TRIG_MODE = INPUT_PULLDOWN;
  static const int     PLC_TRIG_EDGE = RISING;
#else
  static const uint8_t PLC_TRIG_MODE = INPUT_PULLUP;
  static const int     PLC_TRIG_EDGE = FALLING;
#endif

// ── Config ───────────────────────────────────────────────
#define DEBOUNCE_US         5000      // ISR noise filter (5ms)
#define UART_BAUD         115200
#define HEARTBEAT_MS        5000      // USB debug interval
#define RESULT_TIMEOUT_MS  15000      // BUSY watchdog; keep above Jetson normal camera recovery time

// ── UART ─────────────────────────────────────────────────
HardwareSerial JetsonSerial(1);       // UART1 → GPIO16(RX)/GPIO17(TX)

// ── State ────────────────────────────────────────────────
volatile uint32_t triggerCount  = 0;
volatile uint32_t lastTriggerUs = 0;

int lastResultPin = -1;  // Tracks which result pin (OK/NG) is currently held HIGH

char    cmdBuffer[65];
uint8_t cmdLen = 0;

uint32_t      trigTxCount      = 0;
uint32_t      rxCmdCount       = 0;
uint32_t      unknownCmdCount  = 0;
uint32_t      overflowCount    = 0;
uint32_t      busyDropCount    = 0;
uint32_t      timeoutCount     = 0;
unsigned long lastDbgMs        = 0;
bool          busyAsserted     = false;
unsigned long busySinceMs      = 0;

const int kOutputPins[] = { PIN_LIGHT, PIN_OK, PIN_NG, PIN_BUSY };

// ── Helpers ──────────────────────────────────────────────

void setBusy(bool on) {
  if (on) { OPTO_ON(PIN_BUSY);  busyAsserted = true;  busySinceMs = millis(); }
  else    { OPTO_OFF(PIN_BUSY); busyAsserted = false; busySinceMs = 0; }
}

void initPinSafe(int pin) {
  digitalWrite(pin, OPTO_OFF_LEVEL);
  pinMode(pin, OPTO_PIN_MODE);
  OPTO_OFF(pin);
}

void allOff() {
  OPTO_OFF(PIN_LIGHT); OPTO_OFF(PIN_OK); OPTO_OFF(PIN_NG);
  lastResultPin = -1;
  setBusy(false);
}

// Clear previous result — called when new trigger arrives
void clearResult() {
  if (lastResultPin >= 0) {
    OPTO_OFF(lastResultPin);
    lastResultPin = -1;
  }
}

// Set result pin HIGH (level-hold) and release BUSY immediately.
// Pin stays HIGH until next trigger calls clearResult().
// PLC reads OK/NG level any time while BUSY=OFF.
void setResult(int pin) {
  clearResult();         // Clear any previous result
  OPTO_ON(pin);          // Hold result HIGH
  lastResultPin = pin;
  setBusy(false);        // BUSY=OFF → PLC can read result now
}

// ── ISR ──────────────────────────────────────────────────

void IRAM_ATTR onPLCTrigger() {
  uint32_t now = micros();
  if (now - lastTriggerUs > DEBOUNCE_US) {
    triggerCount++;
    lastTriggerUs = now;
  }
}

// ── SETUP ────────────────────────────────────────────────

void setup() {
  for (int pin : kOutputPins) initPinSafe(pin);
  allOff();

  Serial.begin(115200);
  Serial.println("[ESP32-S3] Booting GPIO Bridge (MKE-K01)...");

  JetsonSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);

  pinMode(PLC_TRIGGER, PLC_TRIG_MODE);
  attachInterrupt(digitalPinToInterrupt(PLC_TRIGGER), onPLCTrigger, PLC_TRIG_EDGE);

  delay(100);
  JetsonSerial.println("ESP32 READY");
  JetsonSerial.printf("ESP32 BOOT reason=%d\n", (int)esp_reset_reason());
  JetsonSerial.printf("ESP32 CFG trig=%d(G%d) ok=%d ng=%d busy=%d light=%d mode=LEVEL timeout=%d\n",
                      (int)PLC_TRIGGER_ACTIVE_HIGH, PLC_TRIGGER,
                      PIN_OK, PIN_NG, PIN_BUSY, PIN_LIGHT, RESULT_TIMEOUT_MS);
  Serial.printf("[ESP32-S3] READY RX=%d TX=%d TRIG=%d OK=%d NG=%d BUSY=%d LIGHT=%d\n",
                UART_RX, UART_TX, PLC_TRIGGER, PIN_OK, PIN_NG, PIN_BUSY, PIN_LIGHT);
}

// ── COMMANDS ─────────────────────────────────────────────

void processCommand(const char* cmd) {
  if      (strcmp(cmd, "LIGHT_ON")  == 0) { OPTO_ON(PIN_LIGHT);  rxCmdCount++; }
  else if (strcmp(cmd, "LIGHT_OFF") == 0) { OPTO_OFF(PIN_LIGHT); rxCmdCount++; }
  else if (strcmp(cmd, "OK_ON")     == 0) { setResult(PIN_OK);   rxCmdCount++; }
  else if (strcmp(cmd, "NG_ON")     == 0) { setResult(PIN_NG);   rxCmdCount++; }
  else if (strcmp(cmd, "OK_PULSE")  == 0) { setResult(PIN_OK);   rxCmdCount++; }
  else if (strcmp(cmd, "NG_PULSE")  == 0) { setResult(PIN_NG);   rxCmdCount++; }
  else if (strcmp(cmd, "BUSY_ON")   == 0) { setBusy(true);       rxCmdCount++; }
  else if (strcmp(cmd, "BUSY_OFF")  == 0) { setBusy(false);      rxCmdCount++; }
  else if (strcmp(cmd, "PING")      == 0) { JetsonSerial.println("PONG"); rxCmdCount++; }
  else if (strcmp(cmd, "STATUS")    == 0) {
    JetsonSerial.printf("ESP32 STAT isr=%lu tx=%lu rx=%lu unk=%lu ovf=%lu drop=%lu busy=%d tout=%lu\n",
                        (unsigned long)triggerCount, (unsigned long)trigTxCount,
                        (unsigned long)rxCmdCount, (unsigned long)unknownCmdCount,
                        (unsigned long)overflowCount, (unsigned long)busyDropCount,
                        (int)busyAsserted,
                        (unsigned long)timeoutCount);
    rxCmdCount++;
  }
  else { unknownCmdCount++; JetsonSerial.printf("ESP32 ERR UNKNOWN:%s\n", cmd); }
}

// ── LOOP ─────────────────────────────────────────────────

void loop() {
  // 1. BUSY watchdog — fail-safe OK if Jetson doesn't respond
  if (busyAsserted && millis() - busySinceMs >= RESULT_TIMEOUT_MS) {
    timeoutCount++;
    JetsonSerial.println("ESP32 ERR TIMEOUT->OK");
    setResult(PIN_OK);  // Fail-safe: assert OK + release BUSY
  }

  // 2. Forward PLC triggers to Jetson
  static uint32_t lastSentCount = 0;
  noInterrupts(); uint32_t cnt = triggerCount; interrupts();

  if (cnt != lastSentCount) {
    uint32_t pending = cnt - lastSentCount;
    lastSentCount = cnt;
    if (busyAsserted) {
      busyDropCount += pending;
    } else {
      clearResult();  // Clear previous OK/NG before new cycle
      setBusy(true);
      JetsonSerial.println("TRIGGER");
      trigTxCount++;
      if (pending > 1) busyDropCount += pending - 1;
    }
  }

  // 3. Read commands from Jetson
  while (JetsonSerial.available()) {
    char c = JetsonSerial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) { cmdBuffer[cmdLen] = '\0'; processCommand(cmdBuffer); cmdLen = 0; }
    } else {
      if (cmdLen < sizeof(cmdBuffer) - 1) cmdBuffer[cmdLen++] = c;
      else { cmdLen = 0; overflowCount++; }
    }
  }

  // 5. USB heartbeat
  unsigned long now = millis();
  if (now - lastDbgMs >= HEARTBEAT_MS) {
    Serial.printf("[S3] %lus isr=%lu tx=%lu rx=%lu busy=%d drop=%lu tout=%lu\n",
                  (unsigned long)(now / 1000), (unsigned long)triggerCount,
                  (unsigned long)trigTxCount, (unsigned long)rxCmdCount,
                  (int)busyAsserted, (unsigned long)busyDropCount,
                  (unsigned long)timeoutCount);
    lastDbgMs = now;
  }
}
