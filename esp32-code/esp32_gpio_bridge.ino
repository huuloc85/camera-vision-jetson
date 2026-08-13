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
//       → "OK_ON"/"NG_ON" → OK/NG=ON + BUSY=OFF
//       → OK/NG stays ON until next trigger or CLEAR

// ── Pins (ESP32-S3 MKE-K01) ─────────────────────────────
// ⚠️ S3 restricted: GPIO19/20=USB, GPIO22-32=Flash/PSRAM
#define UART_RX 16 // ← Jetson TX (wire to ESP32 GPIO16 pin)
#define UART_TX 17 // → Jetson RX (wire to ESP32 GPIO17 pin)

#define PLC_TRIGGER 4 // Opto INPUT (riêng) ← PLC trigger

// Opto OUTPUT module channels
#define PIN_OK 5    // CH1 → PLC OK
#define PIN_NG 6    // CH2 → PLC NG
#define PIN_BUSY 7  // CH3 → PLC BUSY

// ── Opto/Relay drive logic ───────────────────────────────
// Set to 0 = active-HIGH push-pull (tested OK with test_uart.ino)
// Set to 1 = active-LOW open-drain (NPN opto)
#define OPTO_ACTIVE_LOW 0

#if OPTO_ACTIVE_LOW
#define OPTO_ON(pin) digitalWrite(pin, LOW)
#define OPTO_OFF(pin) digitalWrite(pin, HIGH)
static const uint8_t OPTO_OFF_LEVEL = HIGH;
static const uint8_t OPTO_PIN_MODE = OUTPUT_OPEN_DRAIN;
#else
#define OPTO_ON(pin) digitalWrite(pin, HIGH)
#define OPTO_OFF(pin) digitalWrite(pin, LOW)
static const uint8_t OPTO_OFF_LEVEL = LOW;
static const uint8_t OPTO_PIN_MODE = OUTPUT;
#endif

// ── Trigger level (1=active HIGH, 0=active LOW/opto pulls GPIO down) ──────────
#define PLC_TRIGGER_ACTIVE_HIGH 1

#if PLC_TRIGGER_ACTIVE_HIGH
static const uint8_t PLC_TRIG_MODE = INPUT_PULLDOWN;
#else
static const uint8_t PLC_TRIG_MODE = INPUT_PULLUP;
#endif

// ── Config ───────────────────────────────────────────────
#define UART_BAUD 115200
#define RESULT_TIMEOUT_MS 15000  // Release BUSY if Jetson stops responding

// ── UART ─────────────────────────────────────────────────
HardwareSerial JetsonSerial(1); // UART1 → GPIO16(RX)/GPIO17(TX)

// ── State ────────────────────────────────────────────────
uint32_t triggerCount = 0;

int lastResultPin = -1; // Tracks which result pin (OK/NG) is currently held HIGH

char cmdBuffer[65];
uint8_t cmdLen = 0;

bool busyAsserted = false;
unsigned long busySinceMs = 0;
bool triggerWasActive = false;

const int kOutputPins[] = {PIN_OK, PIN_NG, PIN_BUSY};

// ── Helpers ──────────────────────────────────────────────

void setBusy(bool on)
{
  if (on)
  {
    OPTO_ON(PIN_BUSY);
    busyAsserted = true;
    busySinceMs = millis();
  }
  else
  {
    OPTO_OFF(PIN_BUSY);
    busyAsserted = false;
    busySinceMs = 0;
  }
}

void initPinSafe(int pin)
{
  digitalWrite(pin, OPTO_OFF_LEVEL);
  pinMode(pin, OPTO_PIN_MODE);
  OPTO_OFF(pin);
}

void allOff()
{
  OPTO_OFF(PIN_OK);
  OPTO_OFF(PIN_NG);
  lastResultPin = -1;
  setBusy(false);
}

// Clear previous result — called when new trigger arrives
void clearResult()
{
  if (lastResultPin >= 0)
  {
    OPTO_OFF(lastResultPin);
    lastResultPin = -1;
  }
}

// Set result pin HIGH and release BUSY immediately.
// Pin stays HIGH until next trigger or CLEAR.
void setResult(int pin)
{
  clearResult(); // Clear any previous result
  OPTO_ON(pin);  // Hold result HIGH
  lastResultPin = pin;
  setBusy(false); // BUSY=OFF → PLC can read result now
}

bool triggerInputActive()
{
  int level = digitalRead(PLC_TRIGGER);
  return PLC_TRIGGER_ACTIVE_HIGH ? (level == HIGH) : (level == LOW);
}

void pollPLCTrigger()
{
  bool active = triggerInputActive();

  if (active)
  {
    if (!triggerWasActive)
    {
      triggerWasActive = true;
      triggerCount++;
    }
    return;
  }

  triggerWasActive = false;
}

// ── SETUP ────────────────────────────────────────────────

void setup()
{
  for (int pin : kOutputPins)
    initPinSafe(pin);
  allOff();

  JetsonSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);

  pinMode(PLC_TRIGGER, PLC_TRIG_MODE);
  triggerWasActive = triggerInputActive();

  delay(100);
  JetsonSerial.println("ESP32 READY");
}

// ── COMMANDS ─────────────────────────────────────────────

void processCommand(const char *cmd)
{
  if (strcmp(cmd, "OK_ON") == 0)
  {
    setResult(PIN_OK);
  }
  else if (strcmp(cmd, "NG_ON") == 0)
  {
    setResult(PIN_NG);
  }
  else if (strcmp(cmd, "OK_PULSE") == 0)
  {
    setResult(PIN_OK);
  }
  else if (strcmp(cmd, "NG_PULSE") == 0)
  {
    setResult(PIN_NG);
  }
  else if (strcmp(cmd, "BUSY_ON") == 0)
  {
    setBusy(true);
  }
  else if (strcmp(cmd, "BUSY_OFF") == 0)
  {
    setBusy(false);
  }
  else if (strcmp(cmd, "CLEAR") == 0)
  {
    clearResult();
    setBusy(false);
  }
  else if (strcmp(cmd, "PING") == 0)
  {
    JetsonSerial.println("PONG");
  }
}

// ── LOOP ─────────────────────────────────────────────────

void loop()
{
  pollPLCTrigger();

  // Fail-safe: never leave the PLC locked in BUSY if Jetson stops responding.
  if (busyAsserted && millis() - busySinceMs >= RESULT_TIMEOUT_MS)
  {
    setResult(PIN_OK);
  }

  // 2. Forward PLC triggers to Jetson
  static uint32_t lastSentCount = 0;
  uint32_t cnt = triggerCount;

  if (cnt != lastSentCount)
  {
    lastSentCount = cnt;
    if (!busyAsserted)
    {
      clearResult(); // Clear previous OK/NG before new cycle
      setBusy(true);
      JetsonSerial.println("TRIGGER");
    }
  }

  // 3. Read commands from Jetson
  while (JetsonSerial.available())
  {
    char c = JetsonSerial.read();
    if (c == '\n' || c == '\r')
    {
      if (cmdLen > 0)
      {
        cmdBuffer[cmdLen] = '\0';
        processCommand(cmdBuffer);
        cmdLen = 0;
      }
    }
    else
    {
      if (cmdLen < sizeof(cmdBuffer) - 1)
        cmdBuffer[cmdLen++] = c;
      else
      {
        cmdLen = 0;
      }
    }
  }
}
