#include <Wire.h>
#include "Adafruit_MPR121.h"

Adafruit_MPR121 cap = Adafruit_MPR121();

void setup() {
  // fast baud for graph flow (60fps+)
  Serial.begin(115200); 

  // init snsr
  if (!cap.begin(0x5A)) {
    Serial.println("Erreur: MPR121 introuvable ! Verifiez le cablage.");
    while (1); // blk if no snsr
  }

  // --- max reactivity cfg ---
  // rst to clear reg
  cap.writeRegister(MPR121_SOFTRESET, 0x63);
  delay(100);

  // sens param (lower = more sens)
  cap.setThresholds(4, 2); 
}

void loop() {
  // rd elec 2 (idx 2)
  // f = filt data, b = bsline (idle state)
  int f = cap.filteredData(2);
  int b = cap.baselineData(2);
  
  // t = touch bin state (mask 0x04 for elec 2)
  int t = (cap.touched() & 0x04) ? 1 : 0;

  // send csv to py script: filt,base,touch
  Serial.print(f);
  Serial.print(",");
  Serial.print(b);
  Serial.print(",");
  Serial.println(t);

  // 10ms = ~100hz, catch fast touch, no com sat
  delay(10); 
}