#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <MIDI.h>
#include <string.h>


// -------------------- LCD --------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// -------------------- MIDI (DIN OUT via Serial1) --------------------
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// -------------------- Switches --------------------
// 6 switches: "SW2 inserted between SW1 and old SW2"
// SW1=D5, SW2=A3, SW3=D6, SW4=D8, SW5=D7, SW6=D9
const uint8_t NUM_SW = 6;
const uint8_t switchPins[NUM_SW] = {5, A3, 6, 8, 7, 9}; // SW1..SW6

// -------------------- Banks --------------------
const uint8_t NUM_BANKS = 4;
uint8_t currentBank = 1; // 1..4

// -------------------- Action Types --------------------
// NOTE: Added ACT_CC_ONE_SHOT (press only) = 6
enum ActionType : uint8_t {
  ACT_NONE        = 0,
  ACT_CC          = 1,  // press + release
  ACT_PC          = 2,  // press only
  ACT_BANK_UP     = 3,  // press only
  ACT_BANK_DOWN   = 4,  // press only
  ACT_CC_TOGGLE   = 5,  // press only (0 <-> vPress)
  ACT_CC_ONE_SHOT = 6   // press only (sends vPress once, NO release)
};

// -------------------- Slot Config --------------------
struct Slot {
  char     label[5];   // 4 chars + '\0'
  uint8_t  type;       // ActionType
  uint8_t  ch;         // 1..16
  uint8_t  num;        // CC# or PC#
  uint8_t  vPress;     // for CC / CC_TOGGLE / CC_ONE_SHOT
  uint8_t  vRelease;   // for CC only
};

// RAM config: [bank][sw]
Slot cfg[NUM_BANKS][NUM_SW];

// runtime toggle states for CC_TOGGLE
bool togState[NUM_BANKS][NUM_SW];

// -------------------- Debounce (no delay) --------------------
const uint16_t DEBOUNCE_MS = 35;
// -------------------- Chord (SW4+SW5 / SW5+SW6) --------------------
// 인덱스: SW1=0, SW2=1, SW3=2, SW4=3, SW5=4, SW6=5
const uint16_t CHORD_WINDOW_MS = 120; // 80~150 사이 취향. 짧을수록 빠름/오검출↑

bool chord45Latched = false;
bool chord56Latched = false;

// SW4~SW6는 "눌림"을 바로 처리하지 않고 잠깐 보류해서 chord 가능성 확인
bool pendingPress[NUM_SW] = {false};
unsigned long pendingAt[NUM_SW] = {0};
bool sentPress[NUM_SW] = {false}; // 실제 press 이벤트가 handleSwitchEvent로 나갔는지

bool rawState[NUM_SW]      = {false};
bool stableState[NUM_SW]   = {false};
bool lastRaw[NUM_SW]       = {false};
unsigned long lastChangeMs[NUM_SW] = {0};

// -------------------- Serial Line Parser (NO String) --------------------
char lineBuf[220];
uint16_t lineLen = 0;

// -------------------- EEPROM Layout --------------------
const uint16_t EEPROM_MAGIC_ADDR = 0;
const uint16_t EEPROM_DATA_ADDR  = 4;   // after magic
const uint32_t EEPROM_MAGIC = 0x4D434646; // 'MCFF'

// Slot stored size:
// label[4], type, ch, num, vPress, vRelease => 4 + 5 = 9 bytes
static inline uint16_t slotAddr(uint8_t bankIdx, uint8_t swIdx){
  const uint16_t SLOT_SIZE = 9;
  uint16_t index = bankIdx * NUM_SW + swIdx;
  return EEPROM_DATA_ADDR + index * SLOT_SIZE;
}

static inline void eepromWriteSlot(uint8_t bankIdx, uint8_t swIdx, const Slot& s){
  uint16_t a = slotAddr(bankIdx, swIdx);
  EEPROM.update(a+0, (uint8_t)s.label[0]);
  EEPROM.update(a+1, (uint8_t)s.label[1]);
  EEPROM.update(a+2, (uint8_t)s.label[2]);
  EEPROM.update(a+3, (uint8_t)s.label[3]);
  EEPROM.update(a+4, s.type);
  EEPROM.update(a+5, s.ch);
  EEPROM.update(a+6, s.num);
  EEPROM.update(a+7, s.vPress);
  EEPROM.update(a+8, s.vRelease);
}

static inline void eepromReadSlot(uint8_t bankIdx, uint8_t swIdx, Slot& s){
  uint16_t a = slotAddr(bankIdx, swIdx);
  s.label[0] = (char)EEPROM.read(a+0);
  s.label[1] = (char)EEPROM.read(a+1);
  s.label[2] = (char)EEPROM.read(a+2);
  s.label[3] = (char)EEPROM.read(a+3);
  s.label[4] = '\0';

  s.type     = EEPROM.read(a+4);
  s.ch       = EEPROM.read(a+5);
  s.num      = EEPROM.read(a+6);
  s.vPress   = EEPROM.read(a+7);
  s.vRelease = EEPROM.read(a+8);

  // sanitize
  if (s.ch < 1 || s.ch > 16) s.ch = 1;
  if (s.type > ACT_CC_ONE_SHOT) s.type = ACT_NONE; // IMPORTANT

  for(int i=0;i<4;i++){
    if (s.label[i] == (char)0xFF || s.label[i] == '\0') s.label[i] = ' ';
  }
  s.label[4] = '\0';
}

static inline void saveAllToEEPROM(){
  EEPROM.put(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  for(uint8_t b=0;b<NUM_BANKS;b++){
    for(uint8_t s=0;s<NUM_SW;s++){
      eepromWriteSlot(b,s,cfg[b][s]);
    }
  }
  Serial.println("OK,SAVED");
}

static inline bool loadAllFromEEPROM(){
  uint32_t m=0;
  EEPROM.get(EEPROM_MAGIC_ADDR, m);
  if(m != EEPROM_MAGIC){
    return false;
  }
  for(uint8_t b=0;b<NUM_BANKS;b++){
    for(uint8_t s=0;s<NUM_SW;s++){
      eepromReadSlot(b,s,cfg[b][s]);
    }
  }
  Serial.println("OK,LOADED");
  return true;
}

// -------------------- LCD Helpers --------------------
static inline void print4(const char* s){
  for(int i=0;i<4;i++){
    char c = s[i];
    if(c == '\0') c = ' ';
    lcd.print(c);
  }
}

static inline void showBankSplash(uint8_t bankNum){
  lcd.clear();
  lcd.setCursor(3,0);
  lcd.print("BANK ");
  lcd.print(bankNum);
  delay(500);
}

static inline void drawLabelsForBank(uint8_t bankNum){
  uint8_t b = bankNum - 1;

  // 16x2 / 4-char labels:
  // row0: SW1(0..3), SW2(6..9), SW3(12..15)
  // row1: SW4(0..3), SW5(6..9), SW6(12..15)
  lcd.clear();

  lcd.setCursor(0,0);  print4(cfg[b][0].label);
  lcd.setCursor(6,0);  print4(cfg[b][1].label);
  lcd.setCursor(12,0); print4(cfg[b][2].label);

  lcd.setCursor(0,1);  print4(cfg[b][3].label);
  lcd.setCursor(6,1);  print4(cfg[b][4].label);
  lcd.setCursor(12,1); print4(cfg[b][5].label);
}

// -------------------- Default Config --------------------
static inline void setDefaultConfig(){
  for(uint8_t b=0;b<NUM_BANKS;b++){
    for(uint8_t s=0;s<NUM_SW;s++){
      cfg[b][s].label[0]='-';
      cfg[b][s].label[1]='-';
      cfg[b][s].label[2]='-';
      cfg[b][s].label[3]='-';
      cfg[b][s].label[4]='\0';

      cfg[b][s].type=ACT_NONE;
      cfg[b][s].ch=1;
      cfg[b][s].num=0;
      cfg[b][s].vPress=127;
      cfg[b][s].vRelease=0;

      togState[b][s]=false;
    }
  }

  // defaults: SW5=DOWN, SW6=UP
  for(uint8_t b=0;b<NUM_BANKS;b++){
    cfg[b][4].type = ACT_BANK_DOWN;
    cfg[b][4].label[0]='D'; cfg[b][4].label[1]='W'; cfg[b][4].label[2]='N'; cfg[b][4].label[3]=' ';

    cfg[b][5].type = ACT_BANK_UP;
    cfg[b][5].label[0]='U'; cfg[b][5].label[1]='P'; cfg[b][5].label[2]=' '; cfg[b][5].label[3]=' ';
  }
}

// -------------------- Execute Action --------------------
static inline void sendCC(uint8_t ch, uint8_t cc, uint8_t val){
  MIDI.sendControlChange(cc, val, ch);
}
static inline void sendPC(uint8_t ch, uint8_t pc){
  MIDI.sendProgramChange(pc, ch);
}

static inline void doBankUp(){
  currentBank++;
  if(currentBank > NUM_BANKS) currentBank = 1;
  showBankSplash(currentBank);
  drawLabelsForBank(currentBank);
}
static inline void doBankDown(){
  if(currentBank <= 1) currentBank = NUM_BANKS;
  else currentBank--;
  showBankSplash(currentBank);
  drawLabelsForBank(currentBank);
}

static inline void handleSwitchEvent(uint8_t swIdx /*0-based*/, bool pressed){
  uint8_t b = currentBank - 1;
  Slot &s = cfg[b][swIdx];

  switch(s.type){
    case ACT_NONE:
      return;

    case ACT_BANK_UP:
      if(pressed) doBankUp();
      return;

    case ACT_BANK_DOWN:
      if(pressed) doBankDown();
      return;

    case ACT_PC:
      if(pressed) sendPC(s.ch, s.num);
      return;

    case ACT_CC:
      // press + release
      sendCC(s.ch, s.num, pressed ? s.vPress : s.vRelease);
      return;

    case ACT_CC_TOGGLE:
      if(pressed){
        togState[b][swIdx] = !togState[b][swIdx];
        uint8_t val = togState[b][swIdx] ? s.vPress : 0;
        sendCC(s.ch, s.num, val);
      }
      return;

    case ACT_CC_ONE_SHOT:
      // press only, NO release
      if(pressed){
        sendCC(s.ch, s.num, s.vPress);
      }
      return;

    default:
      return;
  }
}

// -------------------- Serial Protocol --------------------
// Commands:
// SET,<bank>,<sw>,<label4>,<type>,<ch>,<num>,<vPress>,<vRelease>
// SAVE / LOAD / DUMP
static char* nextTok(char* &p){
  if(!p) return nullptr;
  char* start = p;
  char* comma = strchr(p, ',');
  if(comma){
    *comma = '\0';
    p = comma + 1;
  } else {
    p = nullptr;
  }

  // trim leading spaces
  while(*start == ' ') start++;

  // trim trailing spaces
  int L = (int)strlen(start);
  while(L > 0 && start[L-1] == ' '){
    start[L-1] = '\0';
    L--;
  }
  return start;
}

static inline void dumpConfig(){
  for(uint8_t b=0;b<NUM_BANKS;b++){
    for(uint8_t sw=0;sw<NUM_SW;sw++){
      Slot &s = cfg[b][sw];
      Serial.print("SET,");
      Serial.print(b+1); Serial.print(",");
      Serial.print(sw+1); Serial.print(",");
      Serial.print(s.label); Serial.print(",");
      Serial.print(s.type); Serial.print(",");
      Serial.print(s.ch); Serial.print(",");
      Serial.print(s.num); Serial.print(",");
      Serial.print(s.vPress); Serial.print(",");
      Serial.println(s.vRelease);
    }
  }
  Serial.println("OK,DUMP_DONE");
}

static inline void applySetLine(char* line){
  // line: "SET,...."
  char* p = line + 4;

  char* tBank = nextTok(p);
  char* tSw   = nextTok(p);
  char* tLab  = nextTok(p);
  char* tType = nextTok(p);
  char* tCh   = nextTok(p);
  char* tNum  = nextTok(p);
  char* tVP   = nextTok(p);
  char* tVR   = nextTok(p);

  if(!tBank || !tSw || !tLab || !tType || !tCh || !tNum || !tVP || !tVR){
    Serial.println("ERR,BAD_SET_FORMAT");
    return;
  }

  int bank = atoi(tBank);
  int sw   = atoi(tSw);
  int type = atoi(tType);
  int ch   = atoi(tCh);
  int num  = atoi(tNum);
  int vp   = atoi(tVP);
  int vr   = atoi(tVR);

  if(bank < 1 || bank > NUM_BANKS || sw < 1 || sw > (int)NUM_SW){
    Serial.println("ERR,BAD_INDEX");
    return;
  }

  uint8_t b = (uint8_t)(bank - 1);
  uint8_t s = (uint8_t)(sw - 1);

  // label: '_' -> space, take first 4 chars, pad with space
  int labLen = (int)strlen(tLab);
  for(int i=0;i<4;i++){
    char c = (i < labLen) ? tLab[i] : ' ';
    if(c == '_') c = ' ';
    cfg[b][s].label[i] = c;
  }
  cfg[b][s].label[4] = '\0';

  if(type < 0 || type > ACT_CC_ONE_SHOT) type = ACT_NONE;
  cfg[b][s].type = (uint8_t)type;

  if(ch < 1) ch = 1;
  if(ch > 16) ch = 16;
  cfg[b][s].ch = (uint8_t)ch;

  if(num < 0) num = 0;
  if(num > 127) num = 127;
  cfg[b][s].num = (uint8_t)num;

  if(vp < 0) vp = 0;
  if(vp > 127) vp = 127;
  cfg[b][s].vPress = (uint8_t)vp;

  if(vr < 0) vr = 0;
  if(vr > 127) vr = 127;
  cfg[b][s].vRelease = (uint8_t)vr;

  // reset toggle state whenever config changes
  togState[b][s] = false;

  if(bank == currentBank){
    drawLabelsForBank(currentBank);
  }

  Serial.println("OK,SET");
}

static inline void processSerialLine(char* line){
  if(line[0] == '\0') return;

  if(strncmp(line, "SET,", 4) == 0){
    applySetLine(line);
    return;
  }
  if(strcmp(line, "SAVE") == 0){
    saveAllToEEPROM();
    return;
  }
  if(strcmp(line, "LOAD") == 0){
    if(!loadAllFromEEPROM()){
      Serial.println("ERR,NO_EEPROM_CONFIG");
    } else {
      drawLabelsForBank(currentBank);
    }
    return;
  }
  if(strcmp(line, "DUMP") == 0){
    dumpConfig();
    return;
  }

  Serial.println("ERR,UNKNOWN_CMD");
}

// -------------------- Setup / Loop --------------------
void setup() {
  Serial.begin(115200);

  Serial1.begin(31250);
  MIDI.begin(MIDI_CHANNEL_OMNI);

  lcd.init();
  lcd.backlight();

  // init switches + debounce state
  for(uint8_t i=0;i<NUM_SW;i++){
    pinMode(switchPins[i], INPUT_PULLUP);
    rawState[i] = (digitalRead(switchPins[i]) == LOW);
    stableState[i] = rawState[i];
    lastRaw[i] = rawState[i];
    lastChangeMs[i] = millis();
  }

  setDefaultConfig();
  loadAllFromEEPROM(); // if fails, keep defaults

  drawLabelsForBank(currentBank);
  Serial.println("OK,READY");
}

void loop() {
  // -------- Serial receive (line-based) --------
  while(Serial.available()){
    char c = (char)Serial.read();
    if(c == '\n'){
      if(lineLen >= sizeof(lineBuf)) lineLen = sizeof(lineBuf) - 1;
      lineBuf[lineLen] = '\0';
      if(lineLen > 0 && lineBuf[lineLen-1] == '\r') lineBuf[lineLen-1] = '\0';
      processSerialLine(lineBuf);
      lineLen = 0;
    } else {
      if(lineLen < sizeof(lineBuf)-1){
        lineBuf[lineLen++] = c;
      } else {
        lineLen = 0;
        Serial.println("ERR,LINE_TOO_LONG");
      }
    }
  }

  // -------- Debounce + capture state changes --------
  unsigned long now = millis();

  for(uint8_t i=0;i<NUM_SW;i++){
    rawState[i] = (digitalRead(switchPins[i]) == LOW);

    if(rawState[i] != lastRaw[i]){
      lastRaw[i] = rawState[i];
      lastChangeMs[i] = now;
    }

    if((now - lastChangeMs[i]) >= DEBOUNCE_MS){
      if(stableState[i] != rawState[i]){
        stableState[i] = rawState[i];
        bool pressed = stableState[i];

        // ---- SW4~SW6는 chord 가능성 때문에 "press"를 잠깐 보류 ----
        if(i >= 3 && i <= 5){
          if(pressed){
            pendingPress[i] = true;
            pendingAt[i] = now;
            // press는 아직 안 보냄
          } else {
            // release 발생
            if(sentPress[i]){
              // 이미 press를 보낸 상태면 정상 release 처리
              handleSwitchEvent(i, false);
              sentPress[i] = false;
            } else if(pendingPress[i]){
              // press가 보류 중인데 바로 뗐다 = 짧은 탭
              // chord 아니었으면 "press+release"로 즉시 처리
              pendingPress[i] = false;
              handleSwitchEvent(i, true);
              handleSwitchEvent(i, false);
            } else {
              // 아무것도 안 함
            }
          }
          // chord 판정/실행은 아래에서 한 번에 처리
        } else {
          // ---- SW1~SW3는 즉시 처리 ----
          handleSwitchEvent(i, pressed);
        }
      }
    }
  }

  // -------- Chord 판단 (SW4+SW5=DOWN, SW5+SW6=UP) --------
  bool sw4 = stableState[3];
  bool sw5 = stableState[4];
  bool sw6 = stableState[5];

  bool chord45 = sw4 && sw5; // DOWN
  bool chord56 = sw5 && sw6; // UP

  // chord가 성립하면: pending/단일 액션 싹 다 막고, 뱅크만 실행
  if(chord45){
    if(!chord45Latched){
      chord45Latched = true;

      // 보류/단일 처리 제거 (두 스위치의 개별 MIDI 절대 금지)
      pendingPress[3]=pendingPress[4]=pendingPress[5]=false;
      sentPress[3]=sentPress[4]=sentPress[5]=false;

      doBankDown();
    }
  } else {
    chord45Latched = false;
  }

  if(chord56){
    if(!chord56Latched){
      chord56Latched = true;

      pendingPress[3]=pendingPress[4]=pendingPress[5]=false;
      sentPress[3]=sentPress[4]=sentPress[5]=false;

      doBankUp();
    }
  } else {
    chord56Latched = false;
  }

  // chord 중이면 pendingPress 타이머 처리하지 않음 (개별 액션 금지)
  bool anyChord = chord45 || chord56;
  if(anyChord) return;

  // -------- Chord가 아니면: SW4~SW6 보류 press를 시간 지나면 실행 --------
  for(uint8_t i=3;i<=5;i++){
    if(pendingPress[i]){
      if((now - pendingAt[i]) >= CHORD_WINDOW_MS){
        // chord가 아니고, window도 지났으면 이제 press를 보냄
        pendingPress[i] = false;
        handleSwitchEvent(i, true);
        sentPress[i] = true;
      }
    }
  }
}
