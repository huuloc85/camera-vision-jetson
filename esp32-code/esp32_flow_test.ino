// ESP32-S3 Flow Test - PLC -> ESP32 -> Jetson -> ESP32 -> Opto
// Board: MKE-K01 ESP32-S3 Dev Kit + MKE-B01 IO Shield
//
// Use with scripts/test_flow.py on Jetson. This firmware keeps the same UART
// protocol as the production app: ESP32 sends TRIGGER, Jetson replies OK_ON or
// NG_ON. Diagnostic commands are included for checking each output channel.

#include <esp_system.h>

#define UART_RX       16
#define UART_TX       17
#define PLC_TRIGGER    4

#define PIN_OK         5
#define PIN_NG         6
#define PIN_BUSY       7

// MKE-B01 on this setup is active-HIGH.
#define OPTO_ON(pin)   digitalWrite(pin, HIGH)
#define OPTO_OFF(pin)  digitalWrite(pin, LOW)

// Trigger input: set 1 when the input opto drives GPIO4 high on trigger.
#define PLC_TRIGGER_ACTIVE_HIGH 1

#if PLC_TRIGGER_ACTIVE_HIGH
  static const uint8_t PLC_TRIG_MODE = INPUT_PULLDOWN;
#else
  static const uint8_t PLC_TRIG_MODE = INPUT_PULLUP;
#endif

#define UART_BAUD          115200
#define RESULT_TIMEOUT_MS   15000

HardwareSerial JetsonSerial(1);

volatile uint32_t triggerCount = 0;

char cmdBuffer[65];
uint8_t cmdLen = 0;

uint32_t lastSentCount = 0;
uint32_t trigTxCount = 0;
uint32_t rxCmdCount = 0;
uint32_t unknownCmdCount = 0;
uint32_t busyDropCount = 0;
uint32_t timeoutCount = 0;

bool busyAsserted = false;
unsigned long busySinceMs = 0;
unsigned long lastHeartbeatMs = 0;
int lastResultPin = -1;
bool triggerWasActive = false;

const int kOutputPins[] = { PIN_OK, PIN_NG, PIN_BUSY };

bool triggerInputActive() {
  int level = digitalRead(PLC_TRIGGER);
  return PLC_TRIGGER_ACTIVE_HIGH ? (level == HIGH) : (level == LOW);
}

void pollPLCTrigger() {
  bool active = triggerInputActive();

  if (active) {
    if (!triggerWasActive) {
      triggerWasActive = true;
      triggerCount++;
    }
    return;
  }

  triggerWasActive = false;
}

void setBusy(bool on) {
  if (on) {
    OPTO_ON(PIN_BUSY);
    busyAsserted = true;
    busySinceMs = millis();
  } else {
    OPTO_OFF(PIN_BUSY);
    busyAsserted = false;
    busySinceMs = 0;
  }
}

void clearResult() {
  if (lastResultPin >= 0) {
    OPTO_OFF(lastResultPin);
    lastResultPin = -1;
  }
}

void allOff() {
  OPTO_OFF(PIN_OK);
  OPTO_OFF(PIN_NG);
  clearResult();
  setBusy(false);
}

void setResult(int pin) {
  clearResult();
  OPTO_ON(pin);
  lastResultPin = pin;
  setBusy(false);
}

void pulsePin(int pin, const char* name) {
  Serial.printf("[FLOW] %s GPIO%d pulse\n", name, pin);
  JetsonSerial.printf("%s_ON\n", name);
  OPTO_ON(pin);
  delay(500);
  OPTO_OFF(pin);
  JetsonSerial.printf("%s_OFF\n", name);
}

void sendStatus() {
  JetsonSerial.printf(
    "ESP32 STAT isr=%lu tx=%lu rx=%lu unk=%lu drop=%lu busy=%d timeout=%lu result=%s trig_active=%d armed=%d\n",
    (unsigned long)triggerCount,
    (unsigned long)trigTxCount,
    (unsigned long)rxCmdCount,
    (unsigned long)unknownCmdCount,
    (unsigned long)busyDropCount,
    (int)busyAsserted,
    (unsigned long)timeoutCount,
    lastResultPin == PIN_OK ? "OK" : (lastResultPin == PIN_NG ? "NG" : "NONE"),
    (int)triggerInputActive(),
    (int)!triggerWasActive
  );
}

void processCommand(const char* cmd) {
  Serial.printf("[FLOW] RX '%s'\n", cmd);

  if      (strcmp(cmd, "PING") == 0)      { JetsonSerial.println("PONG"); rxCmdCount++; }
  else if (strcmp(cmd, "STATUS") == 0)    { sendStatus(); rxCmdCount++; }
  else if (strcmp(cmd, "BUSY_ON") == 0)   { setBusy(true); rxCmdCount++; }
  else if (strcmp(cmd, "BUSY_OFF") == 0)  { setBusy(false); rxCmdCount++; }
  else if (strcmp(cmd, "OK_ON") == 0)     { setResult(PIN_OK); rxCmdCount++; }
  else if (strcmp(cmd, "NG_ON") == 0)     { setResult(PIN_NG); rxCmdCount++; }
  else if (strcmp(cmd, "CLEAR") == 0)     { allOff(); rxCmdCount++; }
  else if (strcmp(cmd, "TEST_OK") == 0)   { pulsePin(PIN_OK, "OK"); rxCmdCount++; }
  else if (strcmp(cmd, "TEST_NG") == 0)   { pulsePin(PIN_NG, "NG"); rxCmdCount++; }
  else if (strcmp(cmd, "TEST_BUSY") == 0) { pulsePin(PIN_BUSY, "BUSY"); rxCmdCount++; }
  else if (strcmp(cmd, "TEST_ALL") == 0) {
    const char* names[] = { "OK", "NG", "BUSY" };
    int pins[] = { PIN_OK, PIN_NG, PIN_BUSY };
    JetsonSerial.println("ALL_START");
    for (int i = 0; i < 3; i++) {
      pulsePin(pins[i], names[i]);
      delay(200);
    }
    JetsonSerial.println("ALL_DONE");
    rxCmdCount++;
  }
  else {
    unknownCmdCount++;
    JetsonSerial.printf("ESP32 ERR UNKNOWN:%s\n", cmd);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FLOW] ESP32-S3 flow test starting...");

  for (int pin : kOutputPins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    OPTO_OFF(pin);
  }
  allOff();

  JetsonSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);

  pinMode(PLC_TRIGGER, PLC_TRIG_MODE);
  triggerWasActive = triggerInputActive();

  delay(200);
  JetsonSerial.println("ESP32 READY");
  JetsonSerial.println("FLOW_TEST_READY");
  sendStatus();

  Serial.printf("[FLOW] UART RX=%d TX=%d TRIG=%d active_high=%d OK=%d NG=%d BUSY=%d\n",
                UART_RX, UART_TX, PLC_TRIGGER, PLC_TRIGGER_ACTIVE_HIGH,
                PIN_OK, PIN_NG, PIN_BUSY);
}

void loop() {
  pollPLCTrigger();

  if (busyAsserted && millis() - busySinceMs >= RESULT_TIMEOUT_MS) {
    timeoutCount++;
    JetsonSerial.println("ESP32 ERR TIMEOUT->OK");
    setResult(PIN_OK);
  }
  uint32_t count = triggerCount;

  if (count != lastSentCount) {
    uint32_t pending = count - lastSentCount;
    lastSentCount = count;
    if (busyAsserted) {
      busyDropCount += pending;
    } else {
      clearResult();
      setBusy(true);
      JetsonSerial.println("TRIGGER");
      trigTxCount++;
      if (pending > 1) busyDropCount += pending - 1;
    }
  }

  while (JetsonSerial.available()) {
    char c = JetsonSerial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuffer[cmdLen] = '\0';
        processCommand(cmdBuffer);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuffer) - 1) {
      cmdBuffer[cmdLen++] = c;
    } else {
      cmdLen = 0;
      unknownCmdCount++;
    }
  }

  if (millis() - lastHeartbeatMs >= 5000) {
    Serial.printf("[FLOW] isr=%lu tx=%lu rx=%lu busy=%d drop=%lu\n",
                  (unsigned long)triggerCount,
                  (unsigned long)trigTxCount,
                  (unsigned long)rxCmdCount,
                  (int)busyAsserted,
                  (unsigned long)busyDropCount);
    lastHeartbeatMs = millis();
  }
}
