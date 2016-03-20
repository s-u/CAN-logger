#include <defaults.h>
#include <global.h>
#include <mcp2515.h>
#include <mcp2515_defs.h>

#include <SPI.h>
#include <SD.h>

#define CANSPEED_125    7               // CAN speed at 125 kbps                                                                                       
#define CANSPEED_250    3               // CAN speed at 250 kbps                                                                                       
#define CANSPEED_500    1               // CAN speed at 500 kbps                                                                                       

const int SD_chipSelect = 9;

static int all_ok = 0;
static char fn[32];

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  Serial.println("\n\n\nCAN-Shield v1.0");

  Serial.print("Initializing SD card... ");
  // make sure that the default chip select pin is set to
  // output, even if you don't use it:
  pinMode(SD_chipSelect, OUTPUT);

  // see if the card is present and can be initialized:
  if (!SD.begin(SD_chipSelect)) {
    Serial.println("FAILED - card failed, or not present");
    return;
  }
  Serial.println("OK");

  int i = 0;
  while (1) {
    sprintf(fn, "data%03d.bin", i++);
    if (!SD.exists(fn)) break;
  }
  Serial.print("SD file: ");
  Serial.print(fn);
  Serial.println();

  Serial.print("CAN Init ... ");
  if (mcp2515_init(CANSPEED_500, 3)) // 0 = normal, 3 = read-only
     Serial.println("OK");
  else {
     Serial.println("FAILED");
     return;
  }
  all_ok = 1;
}

typedef struct {
  unsigned long ts;
  tCAN msg;
} log_entry_t;

#define LEB_SIZE 32
static log_entry_t le_buf[LEB_SIZE];
static uint8_t le_pos = 0;

void loop() {
  // put your main code here, to run repeatedly:
  if (!all_ok)
    return;

  File dataFile = SD.open(fn, FILE_WRITE);
  unsigned long last_flush = millis();
  le_pos = 0;

  while (all_ok) {
    if (mcp2515_check_message()) {
      if (mcp2515_get_message(&(le_buf[le_pos].msg))) {
        unsigned long ts = millis();
        le_buf[le_pos++].ts = ts;
        if (le_pos >= LEB_SIZE) {
          dataFile.write((char*)le_buf, sizeof(log_entry_t) * LEB_SIZE);
          le_pos = 0;
        }
        if (ts - last_flush > 2000) { // flush at least after 2s
          if (le_pos) { // something still in the buffer?
            dataFile.write((char*)le_buf, sizeof(log_entry_t) * le_pos);
            le_pos = 0;
          }
          dataFile.flush();
          last_flush = ts;
        }
      }
    }
  }
}

