#include <SoftwareSerial.h>
#include <EEPROM.h>

// ------------------ CONFIGURATION ------------------
#define SIM800_TX 10
#define SIM800_RX 11
#define RELAY_PIN 13

#define MAX_REG_NUMBERS 10
#define MAX_ID_LEN 10
#define SECURITY_CODE "12345"
#define ID_TIMEOUT_MS 180000 // 3 minutes
#define EEPROM_START 0

SoftwareSerial sim800l(SIM800_TX, SIM800_RX);

// ------------------ DATA STRUCTURES ------------------
struct UserEntry {
  char phone[21]; // E.164 format max 20 chars
  char id[MAX_ID_LEN + 1]; // 10 chars + null
};

UserEntry users[MAX_REG_NUMBERS];
uint8_t userCount = 0;

enum State { NORMAL, WAITING_ID };
State regState = NORMAL;
unsigned long idRequestStart = 0;
char pendingPhone[21] = "";

// Relay state
bool relayActive = false;
char lastCaller[21] = "";

// --------------- FUNCTION PROTOTYPES ----------------
void setupSIM800L();
void clearAllSMS();
void saveUsersToEEPROM();
void loadUsersFromEEPROM();
int findUserIndexByPhone(const char* phone);
void handleIncomingSMS();
void handleIncomingCall();
void sendSMS(const char* phone, const char* text);
void activateRelay(const char* byPhone);
void deactivateRelay(const char* byPhone);
void sendStatus(const char* phone);
void sendList(const char* phone);
void resetAllUsers();
bool isSMSStorageFull();
void deleteAllSMS();
void replyAndAskID(const char* phone);
void storePendingUserID(const char* phone, const char* id);
bool isAdmin(const char* phone);

// ------------------ SETUP/LOOP ---------------------
void setup() {
  Serial.begin(9600);
  sim800l.begin(9600);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  loadUsersFromEEPROM();
  setupSIM800L();
  Serial.println("Home Security System Ready");
}

void loop() {
  handleIncomingSMS();
  handleIncomingCall();

  // Handle ID timeout
  if (regState == WAITING_ID && millis() - idRequestStart > ID_TIMEOUT_MS) {
    sim800l.listen();
    sendSMS(pendingPhone, "ID assignment timeout. Registration cancelled.");
    regState = NORMAL;
    pendingPhone[0] = '\0';
  }

  // Periodically check SMS storage and clean if needed
  if (isSMSStorageFull()) {
    deleteAllSMS();
  }
}

// --------------- SIM800L BASIC SETUP ----------------
void setupSIM800L() {
  delay(2000);
  sim800l.println("AT+CMGF=1"); delay(500); // SMS text mode
  sim800l.println("AT+CLIP=1"); delay(500); // Caller ID
  sim800l.println("AT+CNMI=1,2,0,0,0"); delay(500); // Direct SMS to serial
  deleteAllSMS();
}

// --------------- EEPROM USER STORAGE ----------------
void saveUsersToEEPROM() {
  int addr = EEPROM_START;
  EEPROM.write(addr++, userCount);
  for (uint8_t i = 0; i < userCount; i++) {
    for (uint8_t j = 0; j < 21; j++) EEPROM.write(addr++, users[i].phone[j]);
    for (uint8_t j = 0; j < MAX_ID_LEN+1; j++) EEPROM.write(addr++, users[i].id[j]);
  }
}

void loadUsersFromEEPROM() {
  int addr = EEPROM_START;
  userCount = EEPROM.read(addr++);
  if (userCount > MAX_REG_NUMBERS) userCount = 0;
  for (uint8_t i = 0; i < userCount; i++) {
    for (uint8_t j = 0; j < 21; j++) users[i].phone[j] = EEPROM.read(addr++);
    for (uint8_t j = 0; j < MAX_ID_LEN+1; j++) users[i].id[j] = EEPROM.read(addr++);
  }
}

// ----------- USER MANAGEMENT HELPERS ---------------
int findUserIndexByPhone(const char* phone) {
  for (uint8_t i = 0; i < userCount; i++)
    if (strcmp(users[i].phone, phone) == 0)
      return i;
  return -1;
}

bool isAdmin(const char* phone) {
  return (userCount > 0 && strcmp(users[0].phone, phone) == 0);
}

// --------------- SMS RECEPTION ---------------------
void handleIncomingSMS() {
  if (!sim800l.available()) return;
  String data = sim800l.readString();
  if (!data.startsWith("+CMT:")) return;

  int numStart = data.indexOf("\"") + 1;
  int numEnd = data.indexOf("\"", numStart);
  String phone = data.substring(numStart, numEnd);
  phone.trim();
  int msgStart = data.indexOf('\n', numEnd) + 1;
  String message = data.substring(msgStart);
  message.trim();

  char phoneBuf[21];
  phone.toCharArray(phoneBuf, 21);

  // Registration step 1: security code
  if (regState == NORMAL && userCount < MAX_REG_NUMBERS &&
      message.equals(SECURITY_CODE) && findUserIndexByPhone(phoneBuf) == -1) {
    strcpy(pendingPhone, phoneBuf);
    regState = WAITING_ID;
    idRequestStart = millis();
    replyAndAskID(phoneBuf);
    return;
  }
  // Registration step 2: receive ID
  if (regState == WAITING_ID && strcmp(phoneBuf, pendingPhone) == 0) {
    if (message.length() > 0 && message.length() <= MAX_ID_LEN) {
      storePendingUserID(phoneBuf, message.c_str());
      regState = NORMAL;
      pendingPhone[0] = '\0';
      return;
    } else {
      sendSMS(phoneBuf, "Invalid ID. Max 10 chars. Try again.");
      return;
    }
  }
  // Wrong security code
  if (regState == NORMAL && message.equals(SECURITY_CODE) == false &&
      findUserIndexByPhone(phoneBuf) == -1) {
    sendSMS(phoneBuf, "Registration failed: invalid code.");
    return;
  }
  // SMS commands from registered users
  int idx = findUserIndexByPhone(phoneBuf);
  if (idx != -1) {
    if (message.equalsIgnoreCase("status")) {
      sendStatus(phoneBuf);
    } else if (message.equalsIgnoreCase("list")) {
      sendList(phoneBuf);
    } else if (message.equalsIgnoreCase("reset") && isAdmin(phoneBuf)) {
      resetAllUsers();
    }
  }
}

// ----------- REGISTRATION LOGIC -------------
void replyAndAskID(const char* phone) {
  char msg[60];
  snprintf(msg, sizeof(msg), "Registered. Reply with your ID (max %d chars) within 3 minutes.", MAX_ID_LEN);
  sendSMS(phone, msg);
}
void storePendingUserID(const char* phone, const char* id) {
  strcpy(users[userCount].phone, phone);
  strncpy(users[userCount].id, id, MAX_ID_LEN);
  users[userCount].id[MAX_ID_LEN] = '\0';
  userCount++;
  saveUsersToEEPROM();

  char msg[40];
  snprintf(msg, sizeof(msg), "ID saved: %s", id);
  sendSMS(phone, msg);
}

// ------------- CALL HANDLING -----------------
void handleIncomingCall() {
  if (!sim800l.available()) return;
  String data = sim800l.readString();
  if (!data.startsWith("RING")) return;

  // Read incoming number (simulate CLIP)
  String phone = "";
  if (data.indexOf("+CLIP:") != -1) {
    int numStart = data.indexOf("\"") + 1;
    int numEnd = data.indexOf("\"", numStart);
    phone = data.substring(numStart, numEnd);
    phone.trim();
  }
  char phoneBuf[21];
  phone.toCharArray(phoneBuf, 21);

  int idx = findUserIndexByPhone(phoneBuf);
  if (idx == -1) return; // ignore unknown callers

  // Reject call
  sim800l.println("ATH");
  delay(500);

  // Toggle security
  if (!relayActive) {
    activateRelay(phoneBuf);
  } else {
    deactivateRelay(phoneBuf);
  }
}

void activateRelay(const char* byPhone) {
  digitalWrite(RELAY_PIN, HIGH);
  relayActive = true;
  strcpy(lastCaller, byPhone);
  int idx = findUserIndexByPhone(byPhone);

  char msg[50];
  snprintf(msg, sizeof(msg), "Security activated. ID: %s", users[idx].id);
  sendSMS(byPhone, msg);
  if (userCount > 0) sendSMS(users[0].phone, msg); // notify admin
}

void deactivateRelay(const char* byPhone) {
  digitalWrite(RELAY_PIN, LOW);
  relayActive = false;
  strcpy(lastCaller, byPhone);
  int idx = findUserIndexByPhone(byPhone);

  char msg[50];
  snprintf(msg, sizeof(msg), "Security deactivated. ID: %s", users[idx].id);
  sendSMS(byPhone, msg);
  if (userCount > 0) sendSMS(users[0].phone, msg); // notify admin
}

// ------------- STATUS/LIST/RESET -------------
void sendStatus(const char* phone) {
  if (relayActive) sendSMS(phone, "security activated");
  else sendSMS(phone, "security deactivated");
}

void sendList(const char* phone) {
  for (uint8_t i = 0; i < userCount; i++) {
    char msg[50];
    snprintf(msg, sizeof(msg), "%d: %s | ID: %s", i+1, users[i].phone, users[i].id);
    sendSMS(phone, msg);
    delay(1000);
  }
}

void resetAllUsers() {
  userCount = 0;
  saveUsersToEEPROM();
  sendSMS(users[0].phone, "All users reset.");
}

// --------------- SIM800L SMS MGMT -------------
bool isSMSStorageFull() {
  sim800l.println("AT+CPMS?");
  delay(500);
  String r = "";
  while (sim800l.available()) r += sim800l.readString();
  int idx = r.indexOf("+CPMS:");
  if (idx == -1) return false;
  int used = r.substring(idx).toInt();
  return used > 20; // adjust threshold as needed
}
void deleteAllSMS() {
  sim800l.println("AT+CMGD=1,4");
  delay(1000);
}

// --------------- SMS SENDING ------------------
void sendSMS(const char* phone, const char* text) {
  sim800l.print("AT+CMGS=\"");
  sim800l.print(phone);
  sim800l.println("\"");
  delay(200);
  sim800l.print(text);
  sim800l.write(26); // CTRL+Z
  delay(3000);
}