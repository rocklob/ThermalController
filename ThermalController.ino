#include <RunningMedian.h>
#include <LiquidCrystal.h>

/* 
 Thermal Controller (for Sous Vide)

 This controls a device for monitoring and regulating the temperature of a 
 system created for the purpose of sous vide cooking, but could be used for
 other purposes.  The system contains the following components:
 
 * Waterproof Thermistor Probe on analog pin 0 (from eBay seller rogersdini)
 * Solid State Relay on analog pin 5
 * Three tactile switches on pins 6, 7, and 8
 * LED to indicate when in cooking mode on pin 5
 * 16x2 LCD Module with RGB backlight using standard HD44780 chipset
 (http://www.amazon.com/Character-Module-16x2-Backlight-Arduino/dp/B004MGPALC/ref=sr_1_1?ie=UTF8&qid=1330397014&sr=8-1)
   RS on pin 12
   RW on pin 13
   Enable on pin 0
   Data4 on pin 1
   Data5 on pin 2
   Data6 on pin 3
   Data7 on pin 4
   Red Backlight on pin 9
   Green Backlight on pin 10
   Blue Backlight on pin 11  
  
 Created 2012/02/01 by Allen Chen (aec1@cornell.edu)

 Notes:
 Steinhart-Hart equation implementation from:
 http://arduino.cc/playground/ComponentLib/Thermistor2
 
 My eBay thermistor's specs:
   Measurement range: -58 to 302 °F (-50 to 150 °C)
   Cable length (L): 3.5 ft (1m)
   Sensing tip material : water proof sensor  20mmx5mm
   Sensor type: Negative Temperature Coefficient Sensor
   Type: 10K Ohm=25℃  B=3435
   (25 C = 298.15 K)

 */

// Thermistor specifications (B, T0, R0)
//#define EPISCO_K164_10k 4300.0f,298.15f,10000.0f  // episco k164 10k thermistor
#define MY_EBAY_THERM 3435.0f,298.15f,10000.0f // 10k thermistor I got from eBay

// Buttons
enum {
  B_MODE=0,
  B_DOWN,
  B_UP
};
const int BUTTON_COUNT = 3;
const int BUTTON_PIN[3] = {6,7,8};
//const unsigned char BUTTON_MASK[3] = {0b001, 0b010, 0b100};  // replaced with bitwise math.. 1<<i
const int HELD_BUTTON_THRESHOLD = 500; // milliseconds before button is considered held
const int HELD_BUTTON_REPEAT_INTERVAL = 100; // interval for repeat rate when button is held  
const unsigned long DEBOUNCE_TIME = 250;  // minimum time between keypresses for debouncing
// Bit arrays of pressed buttons
unsigned char lastButtons; // pressed in the previous iteration
unsigned char buttons;     // pressed in current iteration
unsigned char pressed;     // newly pressed in this iteration
unsigned char held;        // pressed and held for longer than HELD_BUTTON_THRESHOLD

// Operating modes
enum {
  ABOUT=0,
  SET_TARGET_TEMP,
  SET_THRESHOLD,
  SET_UNITS,
  THERMOMETER,
  TEMP_CONTROL
};
const unsigned int MIN_MODE = SET_TARGET_TEMP;
const unsigned int MAX_MODE = TEMP_CONTROL;
int lastMode;   // mode during previous iteration
int mode;       // current mode

// Temperature scales
enum {
  T_KELVIN=0,
  T_CELSIUS,
  T_FAHRENHEIT
};

// Temperature constants and defaults
const float MAX_TARGET_TEMP = 185.0;
const float MIN_TARGET_TEMP = 40.0;
float targetTemp = 72.0;
float threshold = 1.0;
int units = T_FAHRENHEIT;
RunningMedian thermistorData = RunningMedian(9);

// LCD
const int BACKLIGHT_RED_PIN = 9;
const int BACKLIGHT_GREEN_PIN = 10;
const int BACKLIGHT_BLUE_PIN = 11;
const int THERMISTOR_APIN = 0;
const int SSR_PIN = A5;
const int LCD_COLS = 16;
const int LCD_ROWS = 2;
LiquidCrystal lcd(12,13,0,1,2,3,4);

// SSR
boolean heating = false;     // true if the heat (ssr) is on

// LEDs
const int ACTIVE_LED_PIN = 5;
const int ACTIVE_LED_FADE_INTERVAL = 5;  // ms between LED fading steps

////////////////////////////////////////////////////////////////////////////////

// From what I've read, including stdio is expensive and sprintf doesn't 
// work correctly on some Arduino boards, so here's my way of converting
// a float to a String with given precision
String floatPrecision(float f, int p) {
  String r;
  int whole = int(f);

  // handle negatives
  if (f < 0) {
    r.concat("-");
    f *= -1;
  }

  // get the decimal portion with rounding
  int d = int( int( int(f * pow(10,p+1)) - (whole * pow(10,p+1)) + 5) / 10);
  if (d >= pow(10, p)) {
    whole += 1;
    d -= pow(10, p);
  }

  // whole number portion
  r.concat(whole);
  if (p == 0) {
    return r;
  }
  r.concat(".");

  // This part handles situations where the decimal portion starts with
  // 0s. We know if there aren't enough digits to meet the required 
  // precision, it's because we need to pad it out with some 0s.
  int numDigits;
  if (d == 0) {
    // since we can't do log of 0...
    numDigits = 1;
  } else {
    numDigits = int(log10(d) + 1);
  }
  for (int i = numDigits; i<p; i++) {
    r.concat("0");
  }

  // the remaining decimal
  r.concat(d);

  return r;
}

// Convert fahrenheit to celsius
float f2c(float f) {
  return (f - 32) * 5 / 9;
}

// Given the current temp in fahrenheit, return string with the current temp in
// the selected units
String f2TempString(float f, int u, int precision, boolean showUnit) {
  String t;
  if (u == T_FAHRENHEIT) {
    t.concat(floatPrecision(f, precision));
    if (showUnit) {
      t.concat("F");
    }
  } 
  else {
    t.concat(floatPrecision(f2c(f), precision));
    if (showUnit) {
      t.concat("C");
    }
  }
  return t;
}

// Take milliseconds and convert it to [hh:]mm:ss
String millisToFullTime(unsigned long m) {
  int hours = int(m / 3600000);
  m -= 3600000 * hours;
  int minutes = int(m / 60000);
  m -= 60000 * minutes;
  int seconds = int(m / 1000);
  String ts;
  if (hours > 0) {
    if (hours < 10) {
      ts.concat('0');
    }
    ts.concat(hours);
    ts.concat(":");
  }
  if (minutes < 10) {
    ts.concat("0");
  }
  ts.concat(minutes);
  ts.concat(":");
  if (seconds < 10) {
    ts.concat("0");
  }
  ts.concat(seconds);
  return ts;
}

// Take milliseconds and convert it to mm:ss"s"  or hh:mm"m" if > 1 hour
String millisToShortTime(unsigned long m) {
  int hours = int(m / 3600000);
  m -= 3600000 * hours;
  int minutes = int(m / 60000);
  m -= 60000 * minutes;
  int seconds = int(m / 1000);
  String ts;
  if (hours > 0) {
    if (hours < 10) {
      ts.concat('0');
    }
    ts.concat(hours);
    ts.concat(":");
  }
  if (minutes < 10) {
    ts.concat("0");
  }
  ts.concat(minutes);
  if (hours > 0) {
    ts.concat("m");
    return ts;
  }
  ts.concat(":");
  if (seconds < 10) {
    ts.concat("0");
  }
  ts.concat(seconds);
  ts.concat("s");
  return ts;
}

// Adjust the given temperature in Fahrenheit based on the function derived 
// from manual calibration for this specific thermistor
float adjust_MY_EBAY_THERM(float f) {
  double diff = 0;

  if (f >= 80 &&
    f < 230) {
    diff = 0.0544 * f - 6.4008;
  }
  return f - diff;  
}

// Use Steinhart-Hart equation to calculate temperature based on thermistor 
// properties. This function taken from 
// http://arduino.cc/playground/ComponentLib/Thermistor2
//
// Changed to call my adjustment function at the end.
float Temperature(int AnalogInputNumber,int OutputUnit,float B,float T0,float R0,float R_Balance)
{
  float R,T;

  R=1024.0f*R_Balance/float(analogRead(AnalogInputNumber))-R_Balance;
  T=1.0f/(1.0f/T0+(1.0f/B)*log(R/R0));

  switch(OutputUnit) {
  case T_CELSIUS :
    T-=273.15f;
    break;
  case T_FAHRENHEIT :
    T=9.0f*(T-273.15f)/5.0f+32.0f;
    break;
  default:
    break;
  };

  return adjust_MY_EBAY_THERM(T);
}

// Set the color of the LCD backlight
void backlight(int r, int g, int b) {
  analogWrite(BACKLIGHT_RED_PIN, 255-r);
  analogWrite(BACKLIGHT_GREEN_PIN, 255-g);
  analogWrite(BACKLIGHT_BLUE_PIN, 255-b);
}

// Make the activity LED fade on and off
void activeLed() {
  static int brightness = 0;
  static int fadeBy = 1;
  static unsigned long lastChangeTime = millis();
  static boolean active;
  
  if (mode == TEMP_CONTROL) {
    if (millis() - lastChangeTime > ACTIVE_LED_FADE_INTERVAL) {
      active = true;
      analogWrite(ACTIVE_LED_PIN, 255-brightness);
      brightness += fadeBy;
      if (brightness == 0 || brightness == 255) {
        fadeBy = -fadeBy;
      }
      lastChangeTime = millis();
    }
  } else {
    // turn off LED
    if (active) {
      analogWrite(ACTIVE_LED_PIN, 0);
      active = false;
    }
  }   
}

// Read current button states and return bit array representation
unsigned char getButtonStates() {
  unsigned char buttonBits = 0b0;
  for (int i = 0; i < BUTTON_COUNT; i++) {
    int state = digitalRead(BUTTON_PIN[i]);
    if (state == HIGH) {
      buttonBits |= 1<<i;
    }
  }
  return buttonBits;
}  

// Return true bits for any buttons that were newly pressed
unsigned char newlyPressed() {
  static unsigned long lastKeyPressTimes[BUTTON_COUNT];
  static unsigned char lastPressed = 0b0;
  unsigned char pressed = 0b0;
  for (int i=0; i<BUTTON_COUNT; i++) {
    if (!(lastButtons & 1<<i) &&
      (buttons & 1<<i)) {
      pressed = 1<<i;
    }
  }
  if (lastPressed != pressed) {
    lastPressed = pressed;
    for (int i=0; i<BUTTON_COUNT; i++) {
      if (millis() - lastKeyPressTimes[i] > DEBOUNCE_TIME) {
        lastKeyPressTimes[i] = millis();
      } 
      else {
        // if not enough time since last press, dont consider it newly pressed
        pressed &= ~(1 << i);  // force ith bit = 0
      }
    }
  }
  return pressed;
}

// Return true bits for any buttons that have been pressed and held 
// longer than the hold threshold
unsigned char pressedAndHeld() {
  static unsigned long firstPressTimes[BUTTON_COUNT];
  unsigned char pressed = newlyPressed();
  unsigned char heldBits;
  for (int i=0; i<BUTTON_COUNT; i++) {
    if (pressed & 1<<i) {
      firstPressTimes[i] = millis();
    }
    if (lastButtons & 1<<i &&
      buttons & 1<<i &&
      millis() - firstPressTimes[i] > HELD_BUTTON_THRESHOLD) {
      heldBits |= 1<<i;
    } 
    else {
      heldBits &= ~(1 << i);
    }
  }
  return heldBits; 
}

// Return the appropriate character for the current temperature unit
char* unitChar() {
  if (units == T_FAHRENHEIT) {
    return "F";
  } 
  else if (units == T_CELSIUS) {
    return "C";
  } 
  else if (units == T_KELVIN) {
    return "K";
  }
  return "?";
}

// Advance to the next mode, wrapping back to the first if we are at the end
void nextMode() {
  if (mode == MAX_MODE) {
    mode = SET_TARGET_TEMP;
  } 
  else {
    mode++;
  }
  lcd.clear();
}

void modeAbout() {
  backlight(0,127,255);
  lcd.setCursor(0, 0);
  //         1234567890123456
  lcd.print("Sous Vide Therm.");
  lcd.setCursor(0, 1);
  lcd.print("Controller v.1g");
}

// Handle the SET_TARGET_TEMP mode, which allows the user to set the target
// (setpoint) temperature.
void modeSetTargetTemp() {
  static int lastTargetTemp;
  static unsigned long lastUpdateTime;  // for tracking held buttons
  if (pressed & 1<<B_DOWN ||
    (held & 1<<B_DOWN && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))
    ) {
    targetTemp--;
    if (targetTemp < MIN_TARGET_TEMP) {
      targetTemp = MIN_TARGET_TEMP;
    }
  }
  if (pressed & 1<<B_UP ||
    (held & 1<<B_UP && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))
    ) {
    targetTemp++;
    if (targetTemp > MAX_TARGET_TEMP) {
      targetTemp = MAX_TARGET_TEMP;
    }
  }
  if ((lastTargetTemp != targetTemp) ||
    (lastMode != mode)) {
    lastUpdateTime = millis();
    backlight(200,200,200);
    lcd.setCursor(0, 0);
    lcd.print("Set Target Temp");
    lcd.setCursor(0, 1);
    lcd.print("[-]          [+]");
    lcd.setCursor(6,1);
    lcd.print(f2TempString(targetTemp, units, 0, true));
  }
  lastTargetTemp = targetTemp;
}

// Handle the SET_THRESHOLD mode, which allows the user to set the amount of
// temperature swings allowed between activations of the heating element for
// the purpose of hysteresis.
void modeSetThreshold() {
  static unsigned long lastUpdateTime;  // for tracking held buttons
  static float lastThreshold = -99.99;
  if (lastThreshold != threshold ||
    lastMode != mode) {
    backlight(200,200,200);
    lcd.setCursor(0, 0);
    lcd.print("Set Threshold");
    lcd.setCursor(0, 1);
    lcd.print("[-]          [+]");
    lcd.setCursor(6,1);
    if (units == T_CELSIUS) {
      lcd.print(floatPrecision(threshold*5/9, 2));
    } 
    else {
      lcd.print(floatPrecision(threshold,1));
    }
    lcd.print(unitChar());
    lastThreshold = threshold;
  }
  if (pressed & 1<<B_DOWN ||
    (held & 1<<B_DOWN && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))  
    ) {
    lastUpdateTime = millis();
    lastThreshold = threshold;
    threshold -= 0.1;
    if (threshold < 0.0) {
      threshold = 0;
    }
  }
  if (pressed & 1<<B_UP ||
    (held & 1<<B_UP && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))  
    ) {
    lastUpdateTime = millis();
    lastThreshold = threshold;
    threshold += 0.1;
  }
}

// Handle the SET_UNITS mode, which simply allows the user to switch between
// Fahrenheit and Celcius.
void modeSetUnits() {
  static int last = -9;
  if (last != units ||
    lastMode != mode) {
    backlight(200,200,200);
    lcd.setCursor(0, 0);
    lcd.print("Set Temp Units");
    lcd.setCursor(0, 1);
    lcd.print("[F]          [C]");
    lcd.setCursor(7,1);
    lcd.print(unitChar());
    last = units;
  }
  if (pressed & 1<<B_DOWN) {
    last = units;
    units = T_FAHRENHEIT;
  }
  if (pressed & 1<<B_UP) {
    last = units;
    units = T_CELSIUS;
  }
}

// Handle the THERMOMETER mode, which simply displays the current temperature
// from the probe and a timer.
void modeThermometer() {
  static unsigned long startTime = millis();
  static float lastTemp;
  static float temp;
  static unsigned long lastUpdateTime;
  static int thermometerUnits = units;  // allow separate units for this mode, but default to system units
  if (lastMode != mode) {
    backlight(0,175,0);
    lcd.setCursor(0, 0);
    lcd.print("Thermometer");
  }
  boolean unitsPressed = false;
  // reset timers if down and up are pressed and held simultaneously
  if (held & 1<<B_DOWN &&
    held & 1<<B_UP) {
    startTime = millis();
  } else {
    if (pressed & 1<<B_DOWN) {
      thermometerUnits = T_FAHRENHEIT;
      unitsPressed = true;
    }
    if (pressed & 1<<B_UP) {
      thermometerUnits = T_CELSIUS;
      unitsPressed = true;
    }
  }
  // prevent display from updating too quickly, which causes flicker
  if (!unitsPressed &&
    millis() - lastUpdateTime < 500) {
    return;
  } 
  lastUpdateTime = millis(); 
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  //temp = Temperature(THERMISTOR_APIN,T_FAHRENHEIT,MY_EBAY_THERM,10000.0f);
  temp = thermistorData.getAverage();
  lcd.print(f2TempString(temp, thermometerUnits, 1, true));
  String ts = millisToFullTime(lastUpdateTime - startTime);
  lcd.setCursor(LCD_COLS - ts.length(), 1);
  lcd.print(ts);
}

// Handle the TEMP_CONTROL mode, which is the mode that actually manages the
// heating element to maintain the desired temperature.
void modeTempControl() {
  static unsigned long cookStartTime = millis();
  static unsigned long lastUpdateTime = cookStartTime;
  static unsigned long heatingTime = 0;
  static int lastTargetTemp = targetTemp;
  boolean switched = false;

  //float temp = Temperature(THERMISTOR_APIN,T_FAHRENHEIT,MY_EBAY_THERM,10000.0f);
  float temp = thermistorData.getAverage();
  
  // set the backlight color based on whether the ssr is on or not
  if (lastMode != mode) {
    if (heating) {
      backlight(255, 0, 0);
    } 
    else {
      backlight(200, 64, 0);
    }
  }

  // change the target temp
  if (pressed & 1<<B_DOWN ||
    (held & 1<<B_DOWN && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))
    ) {
    targetTemp--;
    if (targetTemp < MIN_TARGET_TEMP) {
      targetTemp = MIN_TARGET_TEMP;
    }
  }
  if (pressed & 1<<B_UP ||
    (held & 1<<B_UP && 
    (millis() - lastUpdateTime > HELD_BUTTON_REPEAT_INTERVAL))
    ) {
    targetTemp++;
    if (targetTemp > MAX_TARGET_TEMP) {
      targetTemp = MAX_TARGET_TEMP;
    }
  }
  // reset timers if down and up are pressed and held simultaneously
  if (held & 1<<B_DOWN &&
    held & 1<<B_UP) {
    cookStartTime = millis();
    lastUpdateTime = cookStartTime;
    heatingTime = 0;
  }

  // enable/disable the ssr as necessary
  if (heating &&
    temp > targetTemp + threshold) {
    digitalWrite(SSR_PIN, LOW);
    backlight(200,64,0);
    heating = false;
    switched = true;
  }
  if (!heating &&
    temp < targetTemp - threshold) {
    digitalWrite(SSR_PIN, HIGH);
    backlight(255,0,0);
    heating = true;
    switched = true;
  }

  // Refresh display if more than 500ms passed since last refresh,
  // or if the ssr has been switched on or off,
  // or if the timer was just reset,
  // or if the target temperature has changed
  if (millis() - lastUpdateTime > 500 ||
    switched ||
    lastUpdateTime - cookStartTime <= 0 ||
    lastTargetTemp != targetTemp) {
    if (heating) {
      heatingTime += millis() - lastUpdateTime;
    }
    lastUpdateTime = millis();
    lcd.clear();
    lcd.print("S:");
    lcd.print(f2TempString(targetTemp, units, 0, true));
    lcd.setCursor(8,0);
    lcd.print("T:");
    String ts = millisToShortTime(lastUpdateTime - cookStartTime);
    lcd.print(ts);
    lcd.setCursor(0, 1);
    lcd.print("P:");
    lcd.print(f2TempString(temp, units, 1, false));
    lcd.setCursor(8,1);
    lcd.print("H:");
    lcd.print(millisToShortTime(heatingTime));
  }
  lastTargetTemp = targetTemp;
}

void setup() {
  lcd.begin(16,2);
  pinMode (BACKLIGHT_RED_PIN, OUTPUT);
  pinMode (BACKLIGHT_GREEN_PIN, OUTPUT);
  pinMode (BACKLIGHT_BLUE_PIN, OUTPUT);
  pinMode (ACTIVE_LED_PIN, OUTPUT);
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode (BUTTON_PIN[i], INPUT);
  }
  pinMode (SSR_PIN, OUTPUT);
}

void loop() {  
  lastButtons = buttons;
  buttons = getButtonStates();
  pressed = newlyPressed();
  held = pressedAndHeld();
  if (pressed & 1<<B_MODE) {
    nextMode();
  }

  // Sample thermistor
  thermistorData.add(Temperature(THERMISTOR_APIN,T_FAHRENHEIT,MY_EBAY_THERM,10000.0f));
  
  // Make sure heat is off if not in temperature control mode
  if (mode != TEMP_CONTROL &&
    heating) {
    digitalWrite(SSR_PIN, LOW);
    heating = false;
  }
  activeLed();

  switch (mode) {
  case SET_TARGET_TEMP:
    modeSetTargetTemp();
    break;
  case SET_THRESHOLD:
    modeSetThreshold();
    break;
  case SET_UNITS:
    modeSetUnits();
    break;
  case THERMOMETER:
    modeThermometer();
    break;
  case TEMP_CONTROL:
    modeTempControl();
    break;
  default:
    modeAbout();
  }
  lastMode = mode;
}

