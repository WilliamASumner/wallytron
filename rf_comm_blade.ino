//#include <DigitalIO.h> // CHANGE RF24/RF24_config.h line 27: #define SOFTSPI line 28-31: define SPI pins
#include <Tlc5948.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <animation_demos.h> // animation demos
#include <tgraphics.h>

#ifdef ARDUINO_TEENSY40 // Teensy 4.0
// For anyone reading this, I found these by using the 4.0 schematic at the bottom of the
// Teensy 4.x page: https://www.pjrc.com/teensy/schematic.html
// and cross-referencing it with https://www.pjrc.com/teensy/IMXRT1060RM_rev2.pdf
//         SIGNAME      MANUAL PORT NAME    GPIO NAME         IRQ #
//        --------      ----------------    ----------        -----
//        SCLK, 13           B0_03           GPIO2_IO03          82
//        MOSI  11           B0_02           GPIO2_IO02          82
//        MISO  12           B0_01           GPIO2_IO01          82
const int IRQ_PIN  = 6;   // B0_10           GPIO2_IO10          82  ***
const int CE_PIN   = 8;   // B1_00           GPIO2_IO16          83
const int CSN_PIN  = 9;   // B0_11           GPIO2_IO11          82
const int NOTEPIN1 = 3;   // EMC_05          GPIO2_IO03          82
const int NOTEPIN2 = 4;   // EMC_06          GPIO4_IO06          86
const int NOTEPIN3 = 5;   // EMC_08          GPIO4_IO08          86
const int MAG_OUT = 14;   // AD_B1_02        GPIO1_IO18          81  ***

const int RADIO_SPI_SPEED = 1000000; // 1Mhz; MIGHT work with faster clocks, not really important

const int MAG_IRQ   = 81;
const int RADIO_IRQ = 82;

const int MAG_PRIORITY    =   127; //  //
const int RADIO_PRIORITY  =   130; //  ||-- choose these carefully! Most important should be highest (lowest num)
const int FRAME_PRIORITY  =   129; //  //
const int COLUMN_PRIORITY =   128; //  //
#else
#error "Unimplemented"
#endif

/*
       ****  Note Processing   ****
     ------------------------------
        * *       ******      * *
        *  *      *    *      *  *
     ****      **** ****   ****
     ****      **** ****   ****
     ------------------------------
*/

// NOTE: THIS MUST BE CALLED BEFORE setupLeds()!!!!
// The reason is these analogWriteFrequency calls are required for the TLC PWM properly
float freqMultiplier = 256.0; // multiplier used to control octave and adjust for PWM count
//    ^^^ in ESPWM this should be ~512, in normal PWM it should be ~65536
float offFrequency = 9000000; // supersonic (hopefully!)
inline void noteSetup() {
  pinMode(NOTEPIN1, OUTPUT);
  pinMode(NOTEPIN2, OUTPUT);
  pinMode(NOTEPIN3, OUTPUT);

  analogWriteFrequency(NOTEPIN1, offFrequency); // set default frequencies ( PWM_REG -> 120Hz, ES_PWM -> 15Khz)
  analogWriteFrequency(NOTEPIN2, offFrequency);
  analogWriteFrequency(NOTEPIN3, offFrequency);

  analogWrite(NOTEPIN1, 127);
  analogWrite(NOTEPIN2, 127);
  analogWrite(NOTEPIN3, 127);
}

inline void playFreq(int notePin, float noteFreq) {
  analogWriteFrequency(notePin, noteFreq * freqMultiplier);
}

inline void stopFreq(int notePin) {
  analogWriteFrequency(notePin, offFrequency);
  //analogWrite(notePin,0); // do nothing for testing
}

/*
      \
        *****       ********     ****    /
        *****       ********     ********
     -- *****       *****        *********  --
        *****       *****        *********
        *********   ********     ********
     /  *********   ********     ****    \
*/
const int NUM_TLCS = 3;
Tlc5948 tlc;

inline void ledSetup() {
  SPI.begin();
  tlc.begin(true, NUM_TLCS);

  tlc.writeGsBufferSPI16((uint16_t*)colorPalette, 3); // clear out the gs data (likely random)
  tlc.setDcData(Channels::out1, 0xff); // dot correction
  tlc.setBcData(0x7f); // global brightness

  Fctrls fSave = tlc.getFctrlBits();
  fSave &= ~(Fctrls::tmgrst_mask);
  fSave |= Fctrls::tmgrst_mode_1; // set timing reset
  fSave &= ~(Fctrls::dsprpt_mask);
  fSave |= Fctrls::dsprpt_mode_1; // set autodisplay repeat

  fSave &= ~(Fctrls::espwm_mask);
  fSave |= Fctrls::espwm_mode_1; // set ES PWM mode on, basically breaks up
  // long ON/OFF periods into 128 smaller segments
  // with even distribution
  tlc.setFctrlBits(fSave);
  tlc.writeControlBufferSPI();
}

inline void testFlash() {
  // check that the radio and drivers are getting along
  tlc.writeGsBufferSPI16((uint16_t*)(colorPalette + 1), 3);
  delay(500);
  tlc.writeGsBufferSPI16((uint16_t*)colorPalette, 3);
}

/*      TIMING STUFF
           ~~~
         `  ^  `
        '   |__>'
        '       '
         ` ~ ~ `
*/
const uint16_t rowSize = 16;
const uint16_t ringSize = 600; // 60; // aka columns
Pixel* displayBuffer   = (Pixel*)malloc(sizeof(Pixel)  * rowSize * ringSize);

// Display refresh rate notes:
// According to Quora (still need to test): house fan 1300 RPM -> 22 RPS -> ~20 FPS
// 20 FPS means 0.05s per rotation / 60 columns -> 833us per column
// 30 FPS -> 555us per column
// 60 FPS -> 277us per column
// Currently, SPI tlc5948 transfer takes 39us per column! Should work c:

IntervalTimer columnTimer; // how we display at the 'right' time for each column
volatile unsigned int displayColumn = 0;

void columnDisplayIsr() {
  noInterrupts();
  tlc.writeGsBufferSPI16((uint16_t*)(displayBuffer + displayColumn * rowSize ), rowSize * 3); // clear out the gs data (likely random)
  displayColumn = (displayColumn + 1) % ringSize;
  asm("dsb");
  interrupts();
}
const unsigned int defaultFrameDurationUs = 143885; // 600000000;
bool columnTimerRunning = false;
inline void columnTimerSetup() {
  unsigned int defaultColDurationUs = defaultFrameDurationUs / ringSize; // 6.95Hz (144ms) / 120 cols = 600 us / col
  // ^^^ This will get updated by hall-effect
  columnTimer.priority(COLUMN_PRIORITY);
  columnTimer.begin(columnDisplayIsr, defaultColDurationUs); // start timer with a guess-timate of column time
  columnTimerRunning = true;
}

inline void columnTimerStop() {
  columnTimerRunning = false;
  noInterrupts();
  columnTimer.end();
  interrupts();
}

inline void columnTimerRestart() {
  unsigned int defaultColDurationUs = defaultFrameDurationUs / ringSize; // This needs to be adjusted for hall-effect (something other than default)
  columnTimer.begin(columnDisplayIsr, defaultColDurationUs);
  columnTimerRunning = true;
}

/*
     ***********           ***********
     **   +   **           **   -   **
     ***********           ***********
     ************         ************
     *********************************
      ****** Hall-Effect Setup ******
       *****************************
*/

// Estimated rotation of about 30 rotations per s ~> 0.033 s or 33 us / rotation to do stuff in general
// (animation, updating audio etc)
volatile uint32_t prevTimeUs = 0;
volatile uint32_t newDurationUs = 0;
volatile uint32_t tmpDurationUs = 0;
// UNTESTED
void hallIsr() {
  // Create a timer with a duration that will give 60 even segments using the time
  // Because a rotation likely won't go over 1s and micros() overflows at 1hr, we should be ok
  tmpDurationUs = newDurationUs;
  newDurationUs = (micros() - prevTimeUs) / ringSize; // convert elapsed time to col time
  if (newDurationUs < 2 || newDurationUs > 1000) {
    newDurationUs = tmpDurationUs; // restore old value on "bad" calculations, might need a filter for this
  }
  columnTimer.update(newDurationUs); // new columnTimer duration
  displayColumn = 0; // reset to beginning of display
  prevTimeUs = micros(); // reset elapsed time
  asm("dsb"); // wait for ISR bit to be cleared before exiting
}

inline void magSetup() {
  pinMode(MAG_OUT, INPUT_PULLUP);
  NVIC_SET_PRIORITY(MAG_IRQ, MAG_PRIORITY);
  attachInterrupt(digitalPinToInterrupt(MAG_OUT), hallIsr, RISING); // TODO check if there's a better way to do this? VVV
}
// A couple of possible improvements: change VREF for RISING (check manual)
//                                    switch to polling with ADC (would that really be better though?)
//                                    Add Kalman filter to ADC data and trigger on threshold?

/*
                 ***************************
                 *** Keyboard Processing ***
                 ***************************
  +----------------------------------------------------------------+
  |     [q] [w] [e] [r] [t] [y] [u] [i] [o] [p] [[] []] [\]        |
  |       [a] [s] [d] [f] [g] [h] [j] [k] [l] [;] ['] [enter]      |
  | [shift] [z] [x] [c] [v] [b] [n] [m] [,] [.] [/]  [ shift]      |
  | [fn] [ctrl] [alt] [cmd] [      space     ] [cmd] [alt] [ctrl]  |
  +----------------------------------------------------------------+

*/

/* Keys are defined as:
       Col0    Col1
      [ 0 ]   [ 1 ]  // Row 0  \___ Note Group 1
      [ 2 ]   [ 3 ]  // Row 1  /
      -------------
      [ 4 ]   [ 5 ]  ... \_________ Note Group 2
      [ 6 ]   [ 7 ]  ... /
      -------------
      [ 8 ]   [ 9 ]  ...      \____ Note Group 3
      [ 10 ]  [ 11 ] // Row 5 /

*/

// Note list 

const float c0 = 16.35;
const float c_sharp_0_d_flat_0 = 17.32;
const float d0 = 18.35;
const float d_sharp_0_e_flat_0 = 19.45;
const float e0 = 20.60;
const float f0 = 21.83;
const float f_sharp_0_g_flat_0 = 23.12;
const float g0 = 24.50;
const float g_sharp_0_a_flat_0 = 25.96;
const float a0 = 27.50;
const float a_sharp_0_b_flat_0 = 29.14;
const float b0 = 30.87;
const float c1 = 32.70;
const float c_sharp_1_d_flat_1 = 34.65;
const float d1 = 36.71;
const float d_sharp_1_e_flat_1 = 38.89;
const float e1 = 41.20;
const float f1 = 43.65;
const float f_sharp_1_g_flat_1 = 46.25;
const float g1 = 49.00;
const float g_sharp_1_a_flat_1 = 51.91;
const float a1 = 55.00;
const float a_sharp_1_b_flat_1 = 58.27;
const float b1 = 61.74;
const float c2 = 65.41;
const float c_sharp_2_d_flat_2 = 69.30;
const float d2 = 73.42;
const float d_sharp_2_e_flat_2 = 77.78;
const float e2 = 82.41;
const float f2 = 87.31;
const float f_sharp_2_g_flat_2 = 92.50;
const float g2 = 98.00;
const float g_sharp_2_a_flat_2 = 103.83;
const float a2 = 110.00;
const float a_sharp_2_b_flat_2 = 116.54;
const float b2 = 123.47;
const float c3 = 130.81;
const float c_sharp_3_d_flat_3 = 138.59;
const float d3 = 146.83;
const float d_sharp_3_e_flat_3 = 155.56;
const float e3 = 164.81;
const float f3 = 174.61;
const float f_sharp_3_g_flat_3 = 185.00;
const float g3 = 196.00;
const float g_sharp_3_a_flat_3 = 207.65;
const float a3 = 220.00;
const float a_sharp_3_b_flat_3 = 233.08;
const float b3 = 246.94;
const float c4 = 261.63;
const float c_sharp_4_d_flat_4 = 277.18;
const float d4 = 293.66;
const float d_sharp_4_e_flat_4 = 311.13;
const float e4 = 329.63;
const float f4 = 349.23;
const float f_sharp_4_g_flat_4 = 369.99;
const float g4 = 392.00;
const float g_sharp_4_a_flat_4 = 415.30;
const float a4 = 440.00;
const float a_sharp_4_b_flat_4 = 466.16;
const float b4 = 493.88;
const float c5 = 523.25;
const float c_sharp_5_d_flat_5 = 554.37;
const float d5 = 587.33;
const float d_sharp_5_e_flat_5 = 622.25;
const float e5 = 659.25;
const float f5 = 698.46;
const float f_sharp_5_g_flat_5 = 739.99;
const float g5 = 783.99;
const float g_sharp_5_a_flat_5 = 830.61;
const float a5 = 880.00;
const float a_sharp_5_b_flat_5 = 932.33;
const float b5 = 987.77;
const float c6 = 1046.50;
const float c_sharp_6_d_flat_6 = 1108.73;
const float d6 = 1174.66;
const float d_sharp_6_e_flat_6 = 1244.51;
const float e6 = 1318.51;
const float f6 = 1396.91;
const float f_sharp_6_g_flat_6 = 1479.98;
const float g6 = 1567.98;
const float g_sharp_6_a_flat_6 = 1661.22;
const float a6 = 1760.00;
const float a_sharp_6_b_flat_6 = 1864.66;
const float b6 = 1975.53;
const float c7 = 2093.00;
const float c_sharp_7_d_flat_7 = 2217.46;
const float d7 = 2349.32;
const float d_sharp_7_e_flat_7 = 2489.02;
const float e7 = 2637.02;
const float f7 = 2793.83;
const float f_sharp_7_g_flat_7 = 2959.96;
const float g7 = 3135.96;
const float g_sharp_7_a_flat_7 = 3322.44;
const float a7 = 3520.00;
const float a_sharp_7_b_flat_7 = 3729.31;
const float b7 = 3951.07;
const float c8 = 4186.01;
const float c_sharp_8_d_flat_8 = 4434.92;
const float d8 = 4698.63;
const float d_sharp_8_e_flat_8 = 4978.03;
const float e8 = 5274.04;
const float f8 = 5587.65;
const float f_sharp_8_g_flat_8 = 5919.91;
const float g8 = 6271.93;
const float g_sharp_8_a_flat_8 = 6644.88;
const float a8 = 7040.00;
const float a_sharp_8_b_flat_8 = 7458.62;
const float b8 = 7902.13;

// Current frequencies are just two strings an octave apart
const float keysToFreq[12] = {e3, e4,
                              f3, f4,
                              f_sharp_3_g_flat_3, f_sharp_4_g_flat_4,
                              g3, g4,
                              g_sharp_3_a_flat_3, g_sharp_4_a_flat_4,
                              a3, a4
                             };
         
const uint8_t keysToPin[12] = {NOTEPIN1, NOTEPIN1,
                               NOTEPIN1, NOTEPIN1,
                               NOTEPIN2, NOTEPIN2,
                               NOTEPIN2, NOTEPIN2,
                               NOTEPIN3, NOTEPIN3,
                               NOTEPIN3, NOTEPIN3
                              };
const uint8_t keysToGroup[12] = {2, 2, 
                                 2, 2, 
                                 1, 1, 
                                 1, 1, 
                                 0, 0, 
                                 0, 0
                                };
uint8_t groupPressed[3] = {0, 0, 0}; // counters for number of keys
uint16_t keyData = 0x0;
uint16_t prevKeys = 0x0; // holder for previous value of keys
const int nKeys = 12;
const int NO_KEY_CHANGE = 0x1 << nKeys; // TODO check if nKeys > sizeof(int)*8

// TODO share constants through header...

inline uint16_t diffKeys(uint16_t keys) { // timed at 0.24us @ 600Mhz
  if (prevKeys == keys) {
    return NO_KEY_CHANGE; // nothing to do
  }
  uint16_t diff = prevKeys ^ keys;
  prevKeys = keys;
  return diff;
}

void audioUpdate(uint16_t keys, uint16_t diff) {
  if (diff == NO_KEY_CHANGE)
    return;
  for (int i = 0; i < 12; i++) {
    if (diff & 0x1) { // if there's a difference here
      uint8_t pinNum = keysToPin[i];
      uint8_t group = keysToGroup[i];

      // if a key is on and no other note in group playing
      if (keys & 0x1) {
        if (groupPressed[group] == 0) {
          Serial.print("Note on: ");
          Serial.println(keysToFreq[i]);
          playFreq(pinNum, keysToFreq[i]); //note o
        }
        groupPressed[group] += 1;
      } else if (!(keys & 0x1)) {
        if (groupPressed[group] == 1) { // only turn off if a single note is playing
          Serial.println("Note off");
          stopFreq(pinNum);
        }
        groupPressed[group] -= 1;
      }
    }
    diff >>= 1;
    keys >>= 1;
  }

  // Figure out if we need to stop animation (ie a note is playing)
  int notesPlaying = 0;
  for (int i = 0; i < 3; i++) {
    notesPlaying += groupPressed[i];
  }
  if (notesPlaying != 0 && columnTimerRunning) {
    columnTimerStop();
  } else if (notesPlaying == 0 && !columnTimerRunning) {
    columnTimerRestart();
  }
}


// Ring-Buffer and functions for storing keypresses
// Note: This really should use atomic operations... however the time it takes a person
// to make a keypress and send it is *significantly* slower than the time it is to add/push a key
// so I'm leaving it out for now...
// This might create issues with rapid consecutive keypresses or keys pushed "at the same time"
// and if so then atomic ops would probably be the way to fix that
// [keypress1] [keypress2] [keypress3] [keypress4]
const int keyBufferSize = 16; // Actual buffer size
volatile int insertIndex = 0;
volatile int readIndex = 0;
volatile int bufferCount = 0; // Number of valid items in buffer
uint16_t keyBuffer[keyBufferSize] = { 0 };

inline void pushKey(uint16_t key) {
  keyBuffer[insertIndex] = key;
  insertIndex = (insertIndex + 1 ) % keyBufferSize;
  if (bufferCount == keyBufferSize) { // if full
    readIndex = (readIndex + 1 ) % keyBufferSize;
  } else {
    bufferCount++;
  }
}

inline uint16_t popKey() {
  if (bufferCount == 0) { // can only be empty
    return 0x0; // return no key press
  }
  uint16_t tempVal = keyBuffer[readIndex];
  readIndex = (readIndex + 1) % keyBufferSize;
  bufferCount--;
  return tempVal;
}

inline bool keyBufferNotEmpty() {
  return bufferCount > 0;
}

/*                      o
                      //
   ******           //  ******
   ***************************
   ***O**| Radio Setup |**O***
   ***************************
   v                         v
*/

RF24 radio(CE_PIN, CSN_PIN, RADIO_SPI_SPEED);
const uint64_t pipe = 0xB0B15ADEADBEEFAA;

void radioIsr() {
  noInterrupts();
  bool tx_ok, tx_fail, rx_ready;
  uint16_t keyVal = 0x0;
  radio.whatHappened(tx_ok, tx_fail, rx_ready); // figure out why we're getting a call
  if (rx_ready) { // we only want to do sth for new incoming data
    // Read in the value and add it to buffer of things to be processed
    radio.read(&keyVal, sizeof(keyVal));
    pushKey(keyVal);

    // Write the acknowledgement
    // This gives a compiler warning because the newDuration can be changed in an ISR,
    // however it's not catastrophic - this is just to get an idea of rotation speed,
    // so there's no harm if it sends an incorrect value once in a while
    radio.writeAckPayload(1, &newDurationUs, sizeof(newDurationUs));
  }
  asm("dsb"); // wait for the ISR bit to be cleared in case this is too short of an ISR (necessary?)
  interrupts();
}

inline void radioSetup() {
  radio.begin();
  radio.openReadingPipe(1, pipe);
  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate( RF24_250KBPS );

  // Enable Ackpayloads for communicating info back (RPM data, etc)
  radio.enableDynamicPayloads();
  radio.enableAckPayload();
  radio.writeAckPayload(1, &newDurationUs, sizeof(newDurationUs));

  NVIC_SET_PRIORITY(RADIO_IRQ, RADIO_PRIORITY); // set the priority so it isn't too late/take too much cpu time

  radio.startListening();
  delay(200);
  if (radio.isChipConnected()) {
    Serial.println("Radio is connected");
    radio.printPrettyDetails();
  } else {
    Serial.println("Error, radio not available");
  }
  attachInterrupt(digitalPinToInterrupt(IRQ_PIN), radioIsr, FALLING); // IRQ pin goes LOW when it has something to say

}


//  *************************
// *******  Animation  *******
//  *************************

// Animation Ideas:
// Worm (mom)
// Solid-color pulse
// Fireworks
// Oscillating rings

//SimpleFlash myAnim (Colors::RoyalPurple,1000000,10.0);
RainbowWheel myAnim(5.0);
Demo* anim = &myAnim; // Set this to the d-esired animation object

inline void animationSetup() {
  for (uint32_t i = 0; i < ringSize; i++) {
    for (uint32_t j = 0; j < rowSize; j++) {
      displayBuffer[indexAt(rowSize, i, j)] = Colors::Black;
    }
  }
  anim->setup(displayBuffer, rowSize, ringSize);
}

inline void animationUpdate(uint16_t keys, uint16_t diff) {
  if (diff != NO_KEY_CHANGE)
    anim->processKeypress(keys, diff);
  anim->tick();
}

void setup() {
  Serial.begin(9600);

  noteSetup();
  Serial.println("Setup note pins");

  ledSetup();
  Serial.println("Setup LED drivers");

  testFlash();
  Serial.println("Tested LED drivers");

  radioSetup();
  Serial.println("Setup radio");

  // magSetup(); /* DISABLED FOR NOW */
  // Serial.println("Setup magnet sensor");
  animationSetup();
  Serial.println("Setup display buffer");

  columnTimerSetup();
  Serial.println("Setup display timer");

  Serial.println("Finished setup");

  delay(5000);
  /*  Pixel* myArr = (Pixel*)malloc(sizeof(Pixel) * rowSize);
    Pixel col = Colors::Red;
    vecFill(col * 10.0, myArr, rowSize);
    tlc.writeGsBufferSPI16((uint16_t*)(myArr), rowSize * 3);*/

}


// This loop is set up like an event loop of a game
// animationUpdate() is where the graphics happen
// looking at keys/responding to input should be done
// in this loop

void loop() {

  uint16_t keys = NO_KEY_CHANGE, diff = NO_KEY_CHANGE;
  // Get input
  if (keyBufferNotEmpty()) {
    keys = popKey(); // get the new keys
    diff = diffKeys(keys); // figure out keys pressed/unpressed
    audioUpdate(keys, diff); // figure out notes to play
  }
  animationUpdate(keys, diff);
}
