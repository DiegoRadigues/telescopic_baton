#include <RFM69.h>
#include <SPI.h>

// === rf cfg ===
#define NETWORK_ID      0
#define MY_NODE_ID      1
#define TARGET_NODE_ID  2
#define RADIO_FREQ      RF69_868MHZ

#define USE_ENCRYPTION  false
#define ENCRYPT_KEY     "TOPSECRETPASSWRD"

// rfm hw pins
#define RFM_IRQ_PIN     PIN_PA2
#define RFM_CS_PIN      PIN_PA7
#define RFM_RST_PIN     PIN_PB0

// dbg serial (swap to Serial if new brd)
#define DBG_SERIAL      Serial1

// init rf obj
RFM69 radio(RFM_CS_PIN, RFM_IRQ_PIN, true);

// rf hw rst
void resetRadioModule()
{
  pinMode(RFM_RST_PIN, OUTPUT);

  digitalWrite(RFM_RST_PIN, LOW);
  delay(1);

  digitalWrite(RFM_RST_PIN, HIGH);
  delay(2);

  digitalWrite(RFM_RST_PIN, LOW);
  delay(10);
}

// fast bin prnt
void printByteBinary(uint8_t byteValue)
{
  for (int bitIndex = 7; bitIndex >= 0; bitIndex--)
  {
    DBG_SERIAL.print((byteValue >> bitIndex) & 0x01);
  }
}

// === setup ===
void setup()
{
  DBG_SERIAL.begin(115200);
  delay(300);

  DBG_SERIAL.println();
  DBG_SERIAL.println("GATEWAY PAYLOAD READER");

  resetRadioModule();

  SPI.begin();

  bool radioOk = radio.initialize(RADIO_FREQ, MY_NODE_ID, NETWORK_ID);

  if (!radioOk)
  {
    DBG_SERIAL.println("RADIO FAIL");

    // blk if rf fail
    while (1)
    {
      delay(1000);
    }
  }

  radio.setHighPower();

  // crypt if cfg true
  if (USE_ENCRYPTION)
  {
    radio.encrypt(ENCRYPT_KEY);
  }

  DBG_SERIAL.println("RADIO OK");
  DBG_SERIAL.println("Waiting payload...");
}

// === loop ===
void loop()
{
  if (!radio.receiveDone())
  {
    return;
  }

  DBG_SERIAL.print("RX from=");
  DBG_SERIAL.print(radio.SENDERID);

  DBG_SERIAL.print(" len=");
  DBG_SERIAL.print(radio.DATALEN);

  DBG_SERIAL.print(" rssi=");
  DBG_SERIAL.print(radio.RSSI);

  DBG_SERIAL.print(" payload=");

  // dbg format loop
  for (uint8_t dataIndex = 0; dataIndex < radio.DATALEN; dataIndex++)
  {
    uint8_t rxByte = radio.DATA[dataIndex];

    DBG_SERIAL.print(" dec=");
    DBG_SERIAL.print(rxByte);

    DBG_SERIAL.print(" hex=0x");
    if (rxByte < 16) DBG_SERIAL.print('0');
    DBG_SERIAL.print(rxByte, HEX);

    DBG_SERIAL.print(" bin=");
    printByteBinary(rxByte);

    DBG_SERIAL.print(" ");
  }

  // ack tx after rd
  if (radio.ACKRequested())
  {
    radio.sendACK();
  }

  DBG_SERIAL.println();
}