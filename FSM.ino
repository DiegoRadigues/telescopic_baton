#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_MPR121.h"
#include "SparkFunLSM6DS3.h"

// === cfg i2c & snsrs ===
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

// mpr121 touch
Adafruit_MPR121 cap = Adafruit_MPR121();
bool isGripped = false;
unsigned long lastTouchCheck = 0; // non-blk tmr

// bh1750 lux
#define BH1750_ADDR 0x23
#define BH1750_POWER_ON  0x01
#define BH1750_RESET     0x07
#define BH1750_CONT_H_RES_MODE 0x10

const float HOLSTER_LUX_THRESHOLD = 2.0; 
bool isHolstered = false;
unsigned long lastLuxCheck = 0; 

// lsm6ds3 accel
LSM6DS3 myIMU(I2C_MODE, 0x6B);
float lastAccelX = 0.0;
unsigned long lastTimeIMU = 0;
bool isExtended = false; // baton ext status

// flick thresholds
const float SEUIL_ACCEL_X = 5.0;   // g
const float SEUIL_JERK_X  = 400.0;  // g/s

// hit thresholds
const float SEUIL_ACCEL_HIT = 5.0;  // g
const float SEUIL_JERK_HIT  = 400.0; // g/s

// === com cfg: esp32 -> atmega ===
const int DATA_PIN_0 = 4;
const int DATA_PIN_1 = 5;
const int DATA_PIN_2 = 6;
const int SEND_PIN   = 7;

enum DeviceState : uint8_t {
  STATE_INIT          = 0b000,
  STATE_HOLSTER       = 0b001,
  STATE_HAND_CLOSED   = 0b010,
  STATE_HAND_OPEN     = 0b011,
  STATE_GROUND_CLOSED = 0b100,
  STATE_GROUND_OPEN   = 0b101,
  STATE_HIT           = 0b110,
  STATE_ERR           = 0b111
};

// --- bh1750 fx ---
void bh1750Write(uint8_t cmd) {
  Wire.beginTransmission(BH1750_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
}

bool initBH1750() {
  Wire.beginTransmission(BH1750_ADDR);
  if (Wire.endTransmission() != 0) return false;

  bh1750Write(BH1750_POWER_ON);
  delay(10);
  bh1750Write(BH1750_RESET);
  delay(10);
  bh1750Write(BH1750_CONT_H_RES_MODE);
  delay(180); // wait init

  return true;
}

float readLux() {
  Wire.requestFrom(BH1750_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return -1.0f;

  uint16_t raw = Wire.read();
  raw <<= 8;
  raw |= Wire.read();
  
  return raw / 1.2f;
}

// --- com fx ---
void writeStatePins(uint8_t state) {
  state &= 0b00000111;
  digitalWrite(DATA_PIN_0, (state & 0b001) ? HIGH : LOW);
  digitalWrite(DATA_PIN_1, (state & 0b010) ? HIGH : LOW);
  digitalWrite(DATA_PIN_2, (state & 0b100) ? HIGH : LOW);
}

void sendPulse() {
  delay(10); 
  digitalWrite(SEND_PIN, HIGH);
  delay(20); 
  digitalWrite(SEND_PIN, LOW);
  delay(10);
}

void sendState(uint8_t state, const char* stateName) {
  writeStatePins(state);
  Serial.print("Envoi Node -> ");
  Serial.print(stateName);
  Serial.print(" [bin:0b");
  for (int i = 2; i >= 0; i--) {
    Serial.print((state >> i) & 0x01);
  }
  Serial.println("]");
  sendPulse();
}

// === setup ===
void setup() {
  Serial.begin(115200);

  // 1. init com pins
  pinMode(DATA_PIN_0, OUTPUT);
  pinMode(DATA_PIN_1, OUTPUT);
  pinMode(DATA_PIN_2, OUTPUT);
  pinMode(SEND_PIN, OUTPUT);
  writeStatePins(0);
  digitalWrite(SEND_PIN, LOW);

  // 2. i2c cfg
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); 

  // 3. init bh1750
  if (!initBH1750()) {
    Serial.println("Erreur: BH1750 introuvable !");
    while (1);
  }
  Serial.println("BH1750 OK");

  // 4. init mpr121
  if (!cap.begin(0x5A)) {
    Serial.println("Erreur: MPR121 introuvable !");
    while (1);
  }
  cap.writeRegister(MPR121_SOFTRESET, 0x63);
  delay(100);
  cap.setThresholds(4, 2);
  Serial.println("MPR121 OK");

  // 5. init lsm6ds3
  myIMU.settings.accelSampleRate = 416;  
  myIMU.settings.accelRange = 16;        
  myIMU.settings.accelBandWidth = 200;   
  if (myIMU.begin() != 0) {
    Serial.println("Erreur: LSM6DS3 introuvable !");
    while (1);
  }
  Serial.println("LSM6DS3 OK");

  Serial.println("=== SYSTEM READY ===");
  sendState(STATE_INIT, "INIT");
}

// === loop ===
void loop() {
  unsigned long currentMillis = millis();

  // 0. rd mpr121 (non-blk, max 100hz)
  bool currentTouch = isGripped; 
  if (currentMillis - lastTouchCheck >= 10) {
    lastTouchCheck = currentMillis;
    currentTouch = (cap.touched() & 0x10) ? true : false;
  }

  // 1. holster ctrl (non-blk, 200ms)
  if (currentMillis - lastLuxCheck >= 200) {
    lastLuxCheck = currentMillis;
    float lux = readLux();

    if (lux >= 0) {
      // in holster
      if (lux < HOLSTER_LUX_THRESHOLD && !isHolstered) {
        isHolstered = true;
        isExtended = false; // auto fold
        sendState(STATE_HOLSTER, "HOLSTER (Dark)");
      }
      // exit holster
      else if (lux >= HOLSTER_LUX_THRESHOLD && isHolstered) {
        isHolstered = false;
        
        if (currentTouch) {
          isGripped = true;
          if (isExtended) sendState(STATE_HAND_OPEN, "HAND-OPEN (Out of Holster)");
          else sendState(STATE_HAND_CLOSED, "HAND-CLOSED (Out of Holster)");
        } else {
          isGripped = false;
          if (isExtended) sendState(STATE_GROUND_OPEN, "GROUND-OPEN (Out of Holster)");
          else sendState(STATE_GROUND_CLOSED, "GROUND-CLOSED (Out of Holster)");
        }
      }
    }
  }

  // 2. touch ctrl (only if out)
  if (!isHolstered) {
    // grip det
    if (currentTouch && !isGripped) {
      isGripped = true;
      if (isExtended) sendState(STATE_HAND_OPEN, "HAND-OPEN (Grip)");
      else sendState(STATE_HAND_CLOSED, "HAND-CLOSED (Grip)");
    }
    // release det
    else if (!currentTouch && isGripped) {
      isGripped = false;
      if (isExtended) sendState(STATE_GROUND_OPEN, "GROUND-OPEN (Release)");
      else sendState(STATE_GROUND_CLOSED, "GROUND-CLOSED (Release)");
    }
  }

  // 3. imu det (flick & hit - if gripped)
  if (!isHolstered && isGripped) {
    
    float currentAccelX = myIMU.readFloatAccelX();
    float currentAccelY = myIMU.readFloatAccelY();
    float currentAccelZ = myIMU.readFloatAccelZ();
    unsigned long currentMillisIMU = millis();

    if (lastTimeIMU != 0) {
      float dt = (currentMillisIMU - lastTimeIMU) / 1000.0; 
      
      if (dt > 0) {
        float jerkX = (currentAccelX - lastAccelX) / dt;

        // case a: closed -> flick det (open)
        if (!isExtended) {
          if (abs(currentAccelX) > SEUIL_ACCEL_X && abs(jerkX) > SEUIL_JERK_X) {
            if (abs(currentAccelY) < 5.0 && abs(currentAccelZ) < 5.0) {
              isExtended = true; 
              sendState(STATE_HAND_OPEN, "HAND-OPEN (Flick Detecté!)");
              delay(300); // debnc
              currentMillisIMU = millis(); // rst tmr
            }
          }
        }
        // case b: open -> hit det
        else {
          // note: heavy accel on y/z possible
          if (abs(currentAccelX) > SEUIL_ACCEL_HIT && abs(jerkX) > SEUIL_JERK_HIT) {
            
            // 1. send hit
            sendState(STATE_HIT, "HIT (Impact detecté!)");
            
            // 2. hold 0.2s + debnc
            delay(200); 
            
            // 3. rst to open
            sendState(STATE_HAND_OPEN, "HAND-OPEN (Retour post-Hit)");
            
            currentMillisIMU = millis(); // rst tmr to avoid fake jerk
          }
        }
      }
    }
    
    lastAccelX = currentAccelX;
    lastTimeIMU = currentMillisIMU;
  }
}