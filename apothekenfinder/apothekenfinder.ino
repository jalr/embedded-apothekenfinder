#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <ArduinoJson.h>
#include <Ticker.h>

struct PharmacyData {
  const char* name;
  uint32_t start;
  uint32_t end;
  const char* street;
  const char* place;
  const char* number;
};

char ssid[] = "neustadt-aisch.freifunk.net";     //  your network SSID (name)
char pass[] = "";  // your network password

// used for user agent and to display a message if the local pharmacy is on standby
const char thisPharmacyName[] = "Storchen-Apotheke";
const char thisPharmacyPlace[] = "Ipsheim";

char server[] = "apothekenfinder.mobi";
char path[] = "/interface/json.php?device=web&source=not&lat=49.525600&long=10.479913";

#define UPDATE_INTERVAL 1800

#define MAX_DISTANCE 16

uint8_t pageFlipDelay = 15;
#define JSON_MAX_CHARS 5000
#define DRUGSTOREDATA_JSON_SIZE (JSON_OBJECT_SIZE(200))
char json[JSON_MAX_CHARS+1];

uint16_t jsonpos;
uint8_t pageFlipFlag = 0;
PharmacyData pharmacies[10];
uint8_t pharmacyCount = 0;
uint8_t iteration = 0;
int timeZone = 0;

uint8_t status = 0; // 0 = no data, 1 = request started, 2 = got reply, 3 = JSON is valid
uint16_t dataAge = 0;

WiFiClient client;
WiFiUDP Udp;
Ticker pageFlipTicker;
Ticker dataAgeTicker;

// NTP Servers:
IPAddress timeServer(132, 163, 4, 101); // time-a.timefreq.bldrdoc.gov

void increaseDataAge() {
  if (dataAge < 65535)
    dataAge++;
}

void setPageFlipFlag() {
  pageFlipFlag = 1;
}

void moveCursor(uint8_t position) {
  Serial1.write(0x1B); // ESC
  Serial1.write(0x48); // move cursor
  Serial1.write(position); // location
}

void clearScreen() {
  Serial1.write(0x0E); // clear display
  Serial1.write(0x0C); // form feed - cursor to top-left
}

void printWifiStatus() {
  Serial1.print("SSID: ");
  Serial1.print(WiFi.SSID());
  Serial1.print("\r\n");

  IPAddress ip = WiFi.localIP();
  Serial1.print("IP: ");
  Serial1.print(ip);
  Serial1.print("\r\n");

  long rssi = WiFi.RSSI();
  Serial1.print("RSSI: ");
  Serial1.print(rssi);
  Serial1.print(" dBm");
}

void startRequest() {
  Serial.println("\nStarting connection to server...");
  // if you get a connection, report back via serial:
  if (client.connect(server, 80)) {
    memset(json, 0, sizeof(json));
    jsonpos = 0;
    Serial.println("connected to server");
    // Make a HTTP request:
    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.0\r\n");
    client.print("User-Agent: ESP8266/");
    client.print(thisPharmacyName);
    client.print(" ");
    client.print(thisPharmacyPlace);
    client.print("\r\n");
    client.print("Host: ");
    client.print(server);
    client.print("\r\n");
    client.print("Accept: */*\r\n");
    client.print("Referer: http://apothekenfinder.mobi/\r\n");
    client.print("Connection: close\r\n");
	  client.print("\r\n");
  }
}

void getResponse() {
  static char lastchar = 0;
  while (client.available()) {
    Serial.print(".");
    char c = client.read();
    if (jsonpos == 0) {
      if (lastchar == '\n' && c == '\n') {
        json[jsonpos++] = c;
      } else if (c != '\r') {
        lastchar = c;
      }
    } else {
      json[jsonpos++] = c;
    }
  }
}

// FIXME: this function is really ugly but needed as the arduino json lib
// has no support for special characters like german umlauts
void convertUmlauts(char charArray[]) {
  uint16_t j=0;
  for (uint16_t i=0; i<JSON_MAX_CHARS; i++) {
    if ( i<(JSON_MAX_CHARS-5) && charArray[i] == '\\' && charArray[i+1] == 'u' && charArray[i+2] == '0' && charArray[i+3] == '0' ) {
      //Serial.println("unicode character detected:");
      if (charArray[i+4] == 'e' && charArray[i+5] == '4') {
        charArray[j++] = '\xe4';
        i+=5;
      } else
      if (charArray[i+4] == 'f' && charArray[i+5] == 'c') {
        charArray[j++] = '\xfc';
        i+=5;
      } else
      if (charArray[i+4] == 'f' && charArray[i+5] == '6') {
        charArray[j++] = '\xf6';
        i+=5;
      } else
      if (charArray[i+4] == 'd' && charArray[i+5] == 'f') {
        charArray[j++] = '\xdf';
        i+=5;
      } else
      if (charArray[i+4] == 'c' && charArray[i+5] == '4') {
        charArray[j++] = '\xc4';
        i+=5;
      } else
      if (charArray[i+4] == 'd' && charArray[i+5] == 'c') {
        charArray[j++] = '\xdc';
        i+=5;
      } else 
      if (charArray[i+4] == 'd' && charArray[i+5] == '6') {
        charArray[j++] = '\xd6';
        i+=5;
      } else {
        charArray[j++] = charArray[i];
      }
    } else {
      charArray[j++] = charArray[i];
    }
  }
  for (uint16_t i=j; i<JSON_MAX_CHARS; i++) {
    charArray[i] = 0;
  }
}

/*-------- NTP code ----------*/
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

time_t getNtpTime()
{
  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  sendNTPpacket(timeServer);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  // TODO: Fehlerbehandlung
  return 0; // return 0 if unable to get the time
}

void syncTime() {
  Udp.begin(8888);
  setSyncProvider(getNtpTime);
}

void displayPharmacies() {
  uint8_t last_iteration;

  for (uint8_t i = 0; i<pharmacyCount; i++) {
    if (strcmp(pharmacies[i].name, thisPharmacyName)==0 && strcmp(pharmacies[i].place, thisPharmacyPlace)==0 && pharmacies[i].start <= now() && pharmacies[i].end > now()) {
      clearScreen();
      Serial.println(now());
      Serial.println("Diese Apotheke hat Notdienst");
      moveCursor(20);
      Serial1.print("Wir sind heute f\xfcr");
      moveCursor(40);
      Serial1.print("Sie dienstbereit.");
      return;
    }
  }

  if(iteration >= pharmacyCount) {
    iteration = 0;
  }
  last_iteration = iteration;
  
  for(;;) {
    if (pharmacies[iteration].start <= now() && pharmacies[iteration].end > now()) {
      clearScreen();
      pageFlipFlag = 0;
      Serial.println(now());
      Serial1.print(pharmacies[iteration].name);
      Serial.println();
      Serial.println(pharmacies[iteration].name);
      moveCursor(20);
      Serial1.print(pharmacies[iteration].street);
      Serial.println(pharmacies[iteration].street);
      moveCursor(40);
      Serial1.print(pharmacies[iteration].place);
      Serial.println(pharmacies[iteration].place);
      moveCursor(60);
      Serial1.print(pharmacies[iteration].number);
      Serial.println(pharmacies[iteration].number);
      iteration++;
      return;
    }
    if (++iteration >= pharmacyCount) {
      iteration = 0;
    }
    if (iteration == last_iteration) {
      clearScreen();
      Serial1.print("keine aktuellen");
      moveCursor(20);
      Serial1.print("Daten verf\xfcgbar");
      return;
    }
  }
}

bool unserialize(PharmacyData data[], char* json) {
  StaticJsonBuffer<DRUGSTOREDATA_JSON_SIZE> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(json,4);
  if (!root.success()) {
    Serial.println("parseObject() failed");
    return false;
  }
  Serial.println("starting loop");
  JsonArray& result = root["result"].asArray();
  for (JsonArray::iterator it = result.begin(); it != result.end(); ++it) {
    JsonObject& cur = *it;
    uint8_t distance = atoi(cur["distance"]);
    if (distance <= MAX_DISTANCE && atoi(cur["end"]) > now()) {
      Serial.print("pharmacyCount count: ");
      Serial.println(pharmacyCount);
      data[pharmacyCount].name = cur["name"];
      data[pharmacyCount].start = atoi(cur["start"]);
      data[pharmacyCount].end = atoi(cur["end"]);
      data[pharmacyCount].street = cur["street"];
      data[pharmacyCount].place = cur["place"];
      data[pharmacyCount].number = cur["number"];
      pharmacyCount++;
    }
  }
  Serial.print(pharmacyCount);
  Serial.println(" pharmacies");
  return (pharmacyCount > 0);
}

void setup() {
  uint8_t slash = 0;
  Serial.begin(115200, SERIAL_8N1);
  Serial.println("Notdienst ESP8266\n");
  Serial1.begin(9600, SERIAL_8N1);
  Serial1.write(0x1B); // ESC
  Serial1.write(0x49); // software reset
  clearScreen();

  //WiFi.mode(WIFI_AP, WIFI_STA
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial1.print("Verbinden mit WLAN ");
  while (WiFi.status() != WL_CONNECTED) {
    moveCursor(19);
    switch (++slash) {
      case 1:
        Serial1.print("/");
        break;
      case 2:
        Serial1.print("-");
        break;
      case 3:
        Serial1.print("\\");
        break;
      case 4:
        Serial1.print("|");
        break;
      default:
        slash = 0;
    }
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);

    delay(200);
  }
  clearScreen();
  Serial.println("Connected to wifi");
  Serial1.println("Verbunden");
  printWifiStatus();
}

void loop() {
  
  if (status == 0) {
    Serial.println("starting NTP sync");
    syncTime();
    Serial.println("starting HTTP request");
    startRequest();
    status = 1;
  } else if (status == 1) {
    getResponse();
    // if the server's disconnected, stop the client:
    if (!client.connected()) {
      pharmacyCount = 0;
      client.stop();
      status = 2;
      Serial.println();
      Serial.println("got response:");
      Serial.println(json);
      convertUmlauts(json);
      Serial.println(json);
      Serial.println("parsing object");
      if (unserialize(pharmacies, json)) {
        pageFlipTicker.detach();
        dataAgeTicker.detach();
        status = 3;
        dataAge = 0;
        iteration = 0;
        pageFlipFlag = 1;
        pageFlipTicker.attach(pageFlipDelay, setPageFlipFlag);
        dataAgeTicker.attach(1, increaseDataAge);
      } else {
        pageFlipTicker.detach();
        // TODO: Fehlerbehandlung
	    clearScreen();
        status = 0;
      }
    }
  } else if (status == 3) {
    if (dataAge >= UPDATE_INTERVAL) { // Timeout data
      status = 0;
    }
  }
  if (pageFlipFlag == 1) {
    displayPharmacies();
    pageFlipFlag = 0;
  }
}



