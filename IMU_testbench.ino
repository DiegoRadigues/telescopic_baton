#include <Wire.h>
#include "SparkFunLSM6DS3.h"

// init lsm6ds3 i2c (dflt 0x6a)
LSM6DS3 myIMU(I2C_MODE, 0x6A); 

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // flick cfg: max range & high odr to catch fast mvt
  myIMU.settings.accelSampleRate = 416;  // odr hz
  myIMU.settings.accelRange = 16;        // +/-16g range
  myIMU.settings.accelBandWidth = 200;   // filter bw

  // init imu
  if (myIMU.begin() != 0) {
    Serial.println("Erreur I2C : Vérifiez le câblage et les résistances de pull-up.");
    while (1);
  }
  
  // csv hdr for py
  Serial.println("Timestamp,AccelX,AccelY,AccelZ");
}

void loop() {
  // rd tm ms
  unsigned long timestamp = millis();
  
  // rd accel (g)
  float ax = myIMU.readFloatAccelX();
  float ay = myIMU.readFloatAccelY();
  float az = myIMU.readFloatAccelZ();

  // send csv over serial
  Serial.print(timestamp); Serial.print(",");
  Serial.print(ax, 3); Serial.print(",");
  Serial.print(ay, 3); Serial.print(",");
  Serial.println(az, 3);

  // 2ms delay matches ~416hz odr, no buf overflow
  delay(2); 
}