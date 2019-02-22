#include <Arduino.h>
#include <Homie.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266WiFi.h>
#include "FS.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <SPIFFSEditor.h>
//#include <ArduinoOTA.h>

// To upload using curl:
// curl 10.7.10.23/edit --form 'data=@index.htm;filename=/index.htm' -v

#define FW_NAME "ledlocator"
#define FW_VERSION "1.0.0"

#define MAX_NUM_STRIPS 6
#define MAX_LEDS_STRIP 80
#define MAX_NUM_LEDS MAX_NUM_STRIPS *MAX_LEDS_STRIP
#define BRIGHTNESS 255

#define WEB_PORT 80
#define CONFIG_WEB_PORT 88

uint16_t total_strips = 0;
uint16_t strip_sizes[MAX_NUM_STRIPS];
uint16_t total_pixels = 0;

uint8_t pin_allocations[] = {D1, D2, D3, D5, D6, D6};

uint16_t total_pwm_pixels = 0;

Adafruit_NeoPixel strips[MAX_NUM_STRIPS];
Adafruit_NeoPixel neofuncs = Adafruit_NeoPixel(0, -1, NEO_GRB + NEO_KHZ800);

HomieNode stripNode("strip", "strip");

bool dorainbow = false;

struct LEDState
{
  unsigned long remaining;
};

LEDState state[MAX_NUM_STRIPS][MAX_LEDS_STRIP];

AsyncWebServer server(CONFIG_WEB_PORT);
//AsyncWebServer serverStandardPort(WEB_PORT);
AsyncWebSocket ws("/ws");

#define UPDATE_FREQ 40 // 25 FPS
#define HIGHLIGHT_DURATION 10 * 1000
#define HIGHLIGHT_START HIGHLIGHT_DURATION / UPDATE_FREQ
unsigned long nextupdate = UPDATE_FREQ;

uint32_t applyGamma(uint32_t c)
{
  uint8_t r = neofuncs.gamma8((uint8_t)(c >> 16));
  uint8_t g = neofuncs.gamma8((uint8_t)(c >> 8));
  uint8_t b = neofuncs.gamma8((uint8_t)c);
  uint32_t color = (r << 16) + (g << 8) + (b);
  return color;
}

void setPixelColor(uint8_t strip, uint16_t led, uint32_t c)
{
  if (strip >= total_strips)
  {
    return;
  }
  if (led >= strip_sizes[strip])
  {
    return;
  }
  // Apply gamma correction to each color component
  strips[strip].setPixelColor(led, applyGamma(c));
}

void loadStripConfig()
{
  File f = SPIFFS.open("/stripdata.txt", "r");
  if (!f)
  {
    Serial.println("file open failed");
  }
  else
  {
    int i = 0;
    total_pixels = 0;
    while (true)
    {
      String s = f.readStringUntil('\n');
      Serial.printf("Got stripsize: [%s]\n", s.c_str());
      strip_sizes[i] = s.toInt();
      total_pixels += strip_sizes[i];
      Serial.printf("Setting size of strip %d to %d\n", i, strip_sizes[i]);

      // Actually configure the strip
      strips[i].updateLength(strip_sizes[i]);
      strips[i].updateType(NEO_GRB + NEO_KHZ800);
      strips[i].setPin(pin_allocations[i]);
      strips[i].setBrightness(BRIGHTNESS);
      strips[i].begin();
      setPixelColor(i, 0, 0x007700);
      Serial.println("Set.");
      delay(0);
      i++;
      total_strips = i;
      Serial.print("Total strips: ");
      Serial.println(total_strips);
      if (i >= MAX_NUM_STRIPS)
      {
        break;
      }
    }
    f.close();
    // Ensure all unused strips are set to zero size
    while (i < MAX_NUM_STRIPS)
    {
      strip_sizes[i] = 0;
      i++;
    }
  }
}

uint32_t getPixelColor(uint16_t n)
{
  // First figure out the strip to apply this to
  int i = 0;
  while (i < MAX_NUM_STRIPS)
  {
    if (n >= strips[i].numPixels())
    {
      n -= strips[i].numPixels();
      i++;
    }
    else
    {
      break;
    }
  }
  return strips[i].getPixelColor(n);
}

void showAll()
{
  for (int i = 0; i < MAX_NUM_STRIPS; i++)
  {
    if (strip_sizes[i] > 0)
    {
      strips[i].show();
    }
  }
}

// Types of incoming websocket messages - must match index.htm script
#define COLOR_UPDATE String("U\n")
#define SCHEME_UPDATE String("S\n")

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->printf("Hello Client %u :)", client->id());
    client->ping();
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    Serial.printf("ws[%s][%ul] disconnect: \n", server->url(), client->id());
  }
  else if (type == WS_EVT_ERROR)
  {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
  }
  else if (type == WS_EVT_PONG)
  {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
  }
  else if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len)
    {
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if (info->opcode == WS_TEXT)
      {
        String tmp;
        if (msg.startsWith(COLOR_UPDATE))
        {
          uint16_t led;
          uint32_t color;
          msg.remove(0, 2);
          // Calculated from https://arduinojson.org/v5/assistant/
          // plus a bunch of margin, global did not work
          StaticJsonBuffer<(500)> globalJsonBuffer;
          JsonObject &js = globalJsonBuffer.parseObject(msg);
          tmp = js["led"].as<String>();
          led = tmp.toInt();
          tmp = js["color"].as<String>();
        }
        else if (msg.startsWith(SCHEME_UPDATE))
        {
          uint16_t led;
          msg.remove(0, 2);
          led = msg.toInt();
          msg.remove(0, msg.indexOf('\n') + 1);
          String ledtype = msg.substring(0, msg.indexOf('\n'));
          msg.remove(0, msg.indexOf('\n') + 1);
        }
        client->text("OK");
      }
      else
        client->binary("I got your binary message");
    }
    else
    {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0)
      {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len)
      {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final)
        {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I have no idea... some part of the message...");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}

void handleUpdate(AsyncWebServerRequest *request)
{
  Serial.print("Starting: ");
  Serial.println(millis());

  String message = "Updated\n";

  Serial.print("Made Message: ");
  Serial.println(millis());
  uint8_t strip;
  uint16_t led;
  int params = request->params();
  for (int i = 0; i < params; i++)
  {
    AsyncWebParameter *p = request->getParam(i);
    strip = p->name().toInt();
    Serial.print("Setting Strip:");
    Serial.print(strip);
    Serial.print(" LED:");
    led = p->value().toInt();
    Serial.println(led);
    state[strip][led].remaining = HIGHLIGHT_START;
  }

  request->send(200, "text/plain", message);
}

bool stripLedHandler(const HomieRange &range, const String &value)
{
  if (!range.isRange)
    return false; // if it's not a range

  if (range.index < 0 || range.index > total_strips)
    return false; // if it's not a valid range

  String values = value;
  while (values.length() > 0)
  {
    long led = values.toInt();

    if (led < strip_sizes[range.index]){
    state[range.index][led].remaining = HIGHLIGHT_START;
    stripNode.setProperty("strip").setRange(range).send(String(led)); // Update the state of the led
    Serial << "Highlighting Strip: " << range.index << " LED: " << led << endl;
    } else {
      Serial << "LED out of range: "<< led<<endl;
    }

    int nextcomma = values.indexOf(',');
    if (nextcomma < 0)
    {
      break;
    }
    values = values.substring(nextcomma + 1);
  }

  return true;
}

void handleLedCount(AsyncWebServerRequest *request)
{
  String message = String(total_pixels);
  request->send(200, "text/plain", message);
}

void handleRainbow(AsyncWebServerRequest *request)
{
  String message = "Set";
  request->send(200, "text/plain", message);
  Serial.println("Doing Rainbow!");
  dorainbow = true;
}

void handleConfig(AsyncWebServerRequest *request)
{
  char hex[10];
  AsyncJsonResponse *response = new AsyncJsonResponse();
  response->addHeader("Server", "ESP Async Web Server");
  JsonObject &root = response->getRoot();
  root["heap"] = ESP.getFreeHeap();
  root["ssid"] = WiFi.SSID();
  JsonArray &colors = root.createNestedArray("colors");
  for (int i = 0; i < total_pixels; i++)
  {
    //Serial.printf("LED: %d color %08x\n", i, getPixelColor(i));
    sprintf(hex, "%06X", getPixelColor(i));
    colors.add(hex);
  }
  response->setLength();
  request->send(response);
}

byte colprogress(byte start, byte end, float pcg)
{
  int total = end - start;
  byte ret = start + (total * pcg);
  return ret;
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos)
{
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85)
  {
    return neofuncs.Color(255 - WheelPos * 3, 0, WheelPos * 3, 0);
  }
  if (WheelPos < 170)
  {
    WheelPos -= 85;
    return neofuncs.Color(0, WheelPos * 3, 255 - WheelPos * 3, 0);
  }
  WheelPos -= 170;
  return neofuncs.Color(WheelPos * 3, 255 - WheelPos * 3, 0, 0);
}

void updateRGBSchemes(unsigned long now)
{
  for (int strip = 0; strip < total_strips; strip++)
  {
    for (int led = 0; led < strip_sizes[strip]; led++)
    {
      if (state[strip][led].remaining > 1)
      {
        uint32_t color = Wheel((state[strip][led].remaining * 4) % 256);
        if (state[strip][led].remaining < UPDATE_FREQ)
        {
          uint16_t scalebrightness = 255 * state[strip][led].remaining / UPDATE_FREQ;
          uint8_t brightness = (uint8_t)scalebrightness;
          uint8_t r = (uint8_t)(color >> 16);
          uint8_t g = (uint8_t)(color >> 8);
          uint8_t b = (uint8_t)color;
          r = (r * brightness) >> 8;
          g = (g * brightness) >> 8;
          b = (b * brightness) >> 8;
          color = neofuncs.Color(r, g, b);
        }
        setPixelColor(strip, led, color);
        state[strip][led].remaining--;
      }
      else if (state[strip][led].remaining == 1)
      {
        setPixelColor(strip, led, 0);
        state[strip][led].remaining = 0;
      }
    }
  }
}

// Slightly different, this makes the rainbow equally distributed throughout
void rainbowCycle(uint8_t wait)
{
  uint16_t count;

  for (uint16_t j = 0; j < 256 * 5; j++)
  { // 5 cycles of all colors on wheel
    count = 0;
    for (uint8_t strip = 0; strip < total_strips; strip++)
    {
      for (uint16_t led = 0; led < strip_sizes[strip]; led++)
      {
        count++;
        setPixelColor(strip, led, Wheel(((count * 256 / total_pixels) + j) & 255));
      }
    }
    showAll();
    delay(wait);
  }
  for (uint8_t strip = 0; strip < total_strips; strip++)
  {
    for (uint16_t led = 0; led < strip_sizes[strip]; led++)
    {

      setPixelColor(strip, led, 0);
    }
  }
}

void setupWebServer()
{
  // If not in configuration mode, move to the "normal" web port
  // Idea from: https://github.com/me-no-dev/ESPAsyncWebServer/issues/74
  if (Homie.isConfigured())
  {
    // The device is configured, in normal mode
    //server = serverStandardPort; // An attempt to change the port... ¯\_(ツ)_/¯
    if (Homie.isConnected())
    {
      // The device is connected
    }
    else
    {
      // The device is not connected
    }
  }
  else
  {
    // The device is not configured, in either configuration or standalone mode
  }

  // Web Server setup
  server.addHandler(new SPIFFSEditor());

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  server.on("/update", HTTP_GET + HTTP_POST, handleUpdate);
  server.on("/ledcount", HTTP_GET + HTTP_POST, handleLedCount);
  server.on("/rainbow", HTTP_GET + HTTP_POST, handleRainbow);

  server.on("/config", HTTP_GET, handleConfig);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  delay(0);
  server.begin();
  Serial.println("HTTP server started");
  delay(0);
}

void setup()
{
  Serial.begin(115200);
  Serial << endl;

  Serial.println("\n\n");
  Serial.println("Starting...\n\n");

  Homie_setFirmware(FW_NAME, FW_VERSION);
  Homie.disableResetTrigger();

  /*
  Serial.println("Starting SPIFFS...");
  SPIFFS.begin();
  Serial.println("Started SPIFFS");

*/
  stripNode.advertiseRange("strip", 0, MAX_NUM_STRIPS - 1).settable(stripLedHandler);

  Homie.setup();

  loadStripConfig();
  showAll();

  /*
  int counter = 0;

  delay(0);
  WiFi.begin(ssid, password);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    counter++;
    for (uint16_t i = 0; i < counter; i++)
    {
      setPixelColor(0, i, neofuncs.Color(100, 0, 0));
    }
    showAll();
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  for (uint16_t i = 0; i < counter; i++)
  {
    setPixelColor(0, i, neofuncs.Color(0, 0, 0));
  }
  showAll();

*/

  /*
  for (int i = 0; i < 4095; i++)
  {
    Serial.printf("%d : %d\n", i, gamma16(i));
  }
  */

  // XXX Not sure if this will work, but tryingto start web interface after we know if the device isconfigure or not
  setupWebServer();
}

void loop()
{
  Homie.loop();
  unsigned long now = millis();
  /*if (now >= nextupdate)
  {
    Serial.printf("Doing update at %lu, Free Heap: %d\n", millis(), ESP.getFreeHeap());
    delay(0);
    nextupdate = now + UPDATE_FREQ;
  }*/
  if (now >= nextupdate)
  {
    now = now - (now % UPDATE_FREQ);
    updateRGBSchemes(now);
    delay(0);
    showAll();
    delay(0);
    nextupdate = now + UPDATE_FREQ;
  }
  if (dorainbow)
  {
    dorainbow = false;
    rainbowCycle(8);
  }
  //ArduinoOTA.handle();
}
