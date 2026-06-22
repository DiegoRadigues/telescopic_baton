#include <Arduino.h>
#include <RFM69.h>
#include <SPI.h>

// === atmega4809 node gw (esp32 -> rfm69) ===
// wire map:
// esp32 gpi4->d0, gpi5->d1, gpi6->d2, gpi7->send
// rf byte: 0b00BBBFFF (B=batt, F=state)

// === rf cfg ===
#define NETWORK_ID      0
#define MY_NODE_ID      2
#define TO_NODE_ID      1
#define RADIO_FREQ      RF69_868MHZ

// no crypt = faster, rx must match
#define USE_ENCRYPTION  false
#define ENCRYPTION_KEY  "TOPSECRETPASSWRD"

// ack / rtry params
#define ACK_RETRIES     3   // max retries if no ack
#define ACK_WAIT_MS     40  // ack timeout ms

// === rfm69 pins ===
#define PIN_RFM69_IRQ   PIN_PA2
#define PIN_RFM69_CS    PIN_PA7
#define PIN_RFM69_RST   PIN_PB0

// === esp32 bus pins ===
#define PIN_DATA0       PIN_PD0   // bit 0
#define PIN_DATA1       PIN_PD1   // bit 1
#define PIN_DATA2       PIN_PD3   // bit 2
#define PIN_SEND        PIN_PD4   // send isr trigger

#define PIN_BATTERY     PIN_PD5

// === batt cfg ===
const float ADC_VREF_VOLTS = 3.3f;
const float ADC_MAX_VALUE = 1023.0f;
const float BATT_DIVIDER_RATIO = 10.0f / (40.0f + 10.0f);
const float BATT_CRITICAL_VOLTS = 3.50f;

#define CRITICAL_SEND_COUNT      1

// skip rd on each send, too slow
#define BATT_REFRESH_MS          2000UL

#define SHIFT_BATT               3

// === states ===
enum SystemState : uint8_t
{
  INIT_STATE          = 0b000,
  HOLSTER_STATE       = 0b001,
  HAND_CLOSED_STATE   = 0b010,
  HAND_OPEN_STATE     = 0b011,
  GROUND_CLOSED_STATE = 0b100,
  GROUND_OPEN_STATE   = 0b101,
  HIT_STATE           = 0b110,
  ERROR_STATE         = 0b111
};

// === rfm69 object ===
RFM69 radio(PIN_RFM69_CS, PIN_RFM69_IRQ, true);

// === batt vars ===
uint16_t lastBatteryAdc = 0;
float lastBatteryVoltage = 0.0f;
uint8_t lastBatteryLevel = 0;
unsigned long lastBatteryRefreshMs = 0;

bool nodeStopped = false;

// === tx fifo queue ===
// isr grabs bus, loop tx packet
#define SEND_QUEUE_SIZE   8
#define SEND_QUEUE_MASK   (SEND_QUEUE_SIZE - 1)

volatile uint8_t sendQueue[SEND_QUEUE_SIZE];
volatile uint8_t sendHead = 0;
volatile uint8_t sendTail = 0;

// === rfm69 hw rst ===
void resetRfm69()
{
  pinMode(PIN_RFM69_RST, OUTPUT);

  digitalWrite(PIN_RFM69_RST, LOW);
  delay(1);

  digitalWrite(PIN_RFM69_RST, HIGH);
  delay(2);

  digitalWrite(PIN_RFM69_RST, LOW);
  delay(10);
}

// === fast bus rd (no branch) ===
static inline uint8_t readDataBusFast()
{
  uint8_t portValue = VPORTD.IN;

  // pd0,pd1 ok, shift pd3 to bit2
  return (portValue & 0x03) | ((portValue & 0x08) >> 1);
}

// === send isr ===
// no dbg / spi / rf here
void onSendRising()
{
  uint8_t sampledState = readDataBusFast();
  uint8_t nextHead = (sendHead + 1) & SEND_QUEUE_MASK;

  if (nextHead != sendTail)
  {
    sendQueue[sendHead] = sampledState;
    sendHead = nextHead;
  }
}

// === pop event from fifo ===
bool popSendEvent(uint8_t &sampledState)
{
  noInterrupts();

  if (sendHead == sendTail)
  {
    interrupts();
    return false;
  }

  sampledState = sendQueue[sendTail];
  sendTail = (sendTail + 1) & SEND_QUEUE_MASK;

  interrupts();
  return true;
}

// === rd batt adc ===
uint8_t readBattery()
{
  lastBatteryAdc = analogRead(PIN_BATTERY);

  float adcVoltage = ((float)lastBatteryAdc * ADC_VREF_VOLTS) / ADC_MAX_VALUE;
  lastBatteryVoltage = adcVoltage / BATT_DIVIDER_RATIO;

  if (lastBatteryVoltage >= 4.15f) lastBatteryLevel = 7;
  else if (lastBatteryVoltage >= 4.05f) lastBatteryLevel = 6;
  else if (lastBatteryVoltage >= 3.93f) lastBatteryLevel = 5;
  else if (lastBatteryVoltage >= 3.82f) lastBatteryLevel = 4;
  else if (lastBatteryVoltage >= 3.72f) lastBatteryLevel = 3;
  else if (lastBatteryVoltage >= 3.62f) lastBatteryLevel = 2;
  else if (lastBatteryVoltage >= 3.50f) lastBatteryLevel = 1;
  else lastBatteryLevel = 0;

  return lastBatteryLevel;
}

// === tx 1 byte (rf ack/rtry) ===
void sendStatePacket(uint8_t stateBits, uint8_t batteryLevel)
{
  uint8_t statePacket = (stateBits & 0x07) | ((batteryLevel & 0x07) << SHIFT_BATT);
  
  // sendWithRetry blks for ack + rtry if fail
  radio.sendWithRetry(TO_NODE_ID, &statePacket, 1, ACK_RETRIES, ACK_WAIT_MS);
}

// === low batt emergency halt ===
void stopNodeCriticalLowBattery()
{
  for (uint8_t i = 0; i < CRITICAL_SEND_COUNT; i++)
  {
    sendStatePacket(ERROR_STATE, 0);
    delay(10);
  }

  radio.sleep();
  nodeStopped = true;

  while (1)
  {
    delay(1000);
  }
}

// === init send isr ===
void setupSendInterrupt()
{
  pinMode(PIN_DATA0, INPUT);
  pinMode(PIN_DATA1, INPUT);
  pinMode(PIN_DATA2, INPUT);
  pinMode(PIN_SEND, INPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_SEND), onSendRising, RISING);
}

// === setup ===
void setup()
{
  pinMode(PIN_BATTERY, INPUT);

  // adc ref from vdd
  analogReference(VDD);
  delay(5);

  // first rd garbage, clear it
  (void)analogRead(PIN_BATTERY);

  readBattery();
  lastBatteryRefreshMs = millis();

  setupSendInterrupt();
  resetRfm69();
  SPI.begin();

  if (!radio.initialize(RADIO_FREQ, MY_NODE_ID, NETWORK_ID))
  {
    while (1)
    {
      delay(1000);
    }
  }

  radio.setHighPower();

  if (USE_ENCRYPTION)
  {
    radio.encrypt(ENCRYPTION_KEY);
  }

  if (lastBatteryVoltage < BATT_CRITICAL_VOLTS)
  {
    stopNodeCriticalLowBattery();
  }
}

// === loop ===
void loop()
{
  if (nodeStopped)
  {
    return;
  }

  // periodical batt check
  unsigned long now = millis();

  if ((now - lastBatteryRefreshMs) >= BATT_REFRESH_MS)
  {
    lastBatteryRefreshMs = now;
    readBattery();

    if (lastBatteryVoltage < BATT_CRITICAL_VOLTS)
    {
      stopNodeCriticalLowBattery();
    }
  }

  uint8_t receivedState;

  // flush fifo events to rf
  while (popSendEvent(receivedState))
  {
    sendStatePacket(receivedState, lastBatteryLevel);
  }
}