#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =====================================================
// PIN DEFINITIONS
// =====================================================

#define DHTPIN      13
#define DHTTYPE     DHT11

#define SOIL_PIN    34
#define LDR_PIN     35

#define MOTOR_PIN   5
#define ON_LED_PIN  18
#define OFF_LED_PIN 19

// =====================================================
// WIFI
// =====================================================

const char* ssid = "A";
const char* password = "1234567890";

// =====================================================
// TELEGRAM
// =====================================================

#define BOT_TOKEN "8425023253:AAH7C3LhmFN7afmcIgeqcmgnpNNRX78B0Z0"
#define OWNER_CHAT_ID "5384291999"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// =====================================================
// DHT
// =====================================================

DHT dht(DHTPIN, DHTTYPE);

// =====================================================
// SENSOR DATA
// =====================================================

struct SensorData
{
  float temperature;
  float humidity;
  float soilMoisture;
  float lightIntensity;
};

SensorData sensorData;

// Mutex for protecting sensorData
SemaphoreHandle_t sensorMutex;

// =====================================================
// FLAGS
// =====================================================

volatile bool motorState = false;
volatile bool lowSoilWarning = false;

// =====================================================
// TASK HANDLES
// =====================================================

TaskHandle_t sensorTaskHandle;
TaskHandle_t telegramTaskHandle;
TaskHandle_t motorTaskHandle;

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

void sensorTask(void *parameter);
void telegramTask(void *parameter);
void motorTask(void *parameter);

void handleNewMessages(int numNewMessages);

void sendReadings(String chat_id);
void sendStartMessage(String chat_id);

// =====================================================
// SENSOR TASK
// =====================================================

void sensorTask(void *parameter)
{
  while (true)
  {
    // -----------------------------
    // DHT11
    // -----------------------------

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();

    // -----------------------------
    // SOIL MOISTURE
    // -----------------------------

    int soilRaw = analogRead(SOIL_PIN);

    float mois = map(soilRaw, 4095, 1500, 0, 100);
    mois = constrain(mois, 0, 100);

    // -----------------------------
    // LDR
    // -----------------------------

    int lightRaw = analogRead(LDR_PIN);

    float voltage = lightRaw * (3.3 / 4095.0);

    float lux = 0;

    // Prevent division by zero
    if (voltage > 0.01)
    {
      float ldrResistance =
          (3.3 - voltage) * 10000.0 / voltage;

      float ldrResistanceK =
          ldrResistance / 1000.0;

      if (ldrResistanceK > 0)
      {
        lux = 500.0 / ldrResistanceK;
      }
    }

    // -----------------------------
    // UPDATE SHARED DATA
    // -----------------------------

    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
    {
      if (!isnan(temp))
        sensorData.temperature = temp;

      if (!isnan(hum))
        sensorData.humidity = hum;

      sensorData.soilMoisture = mois;
      sensorData.lightIntensity = lux;

      xSemaphoreGive(sensorMutex);
    }

    // -----------------------------
    // LOW SOIL FLAG
    // -----------------------------

    if (mois < 40)
    {
      lowSoilWarning = true;
    }
    else
    {
      lowSoilWarning = false;
    }

    // Read DHT11 every 2 seconds
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// =====================================================
// MOTOR TASK
// =====================================================

void motorTask(void *parameter)
{
  while (true)
  {
    float soil;

    // Get soil moisture safely
    if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
    {
      soil = sensorData.soilMoisture;
      xSemaphoreGive(sensorMutex);
    }

    // -------------------------------------------------
    // AUTOMATIC MOTOR OFF WHEN SOIL > 90%
    // -------------------------------------------------

    if (soil > 90)
    {
      digitalWrite(MOTOR_PIN, LOW);

      motorState = false;

      digitalWrite(ON_LED_PIN, LOW);
      digitalWrite(OFF_LED_PIN, HIGH);
    }

    // -------------------------------------------------
    // LOW SOIL
    // -------------------------------------------------

    if (soil < 40)
    {
      // Only warning is generated.
      // Motor is NOT automatically switched ON,
      // preserving your original program behavior.
    }

    // Check every 500 ms
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =====================================================
// TELEGRAM TASK
// =====================================================

void telegramTask(void *parameter)
{
  unsigned long lastBotCheck = 0;

  while (true)
  {
    // -------------------------------------------------
    // CHECK TELEGRAM MESSAGES
    // -------------------------------------------------

    if (millis() - lastBotCheck > 1000)
    {
      int numNewMessages =
          bot.getUpdates(bot.last_message_received + 1);

      while (numNewMessages)
      {
        handleNewMessages(numNewMessages);

        numNewMessages =
            bot.getUpdates(bot.last_message_received + 1);
      }

      lastBotCheck = millis();
    }

    // -------------------------------------------------
    // LOW SOIL WARNING
    // -------------------------------------------------

    static unsigned long lastWarning = 0;

    if (lowSoilWarning &&
        millis() - lastWarning > 10000)
    {
      bot.sendMessage(
          OWNER_CHAT_ID,
          "⚠️ WATER LEVEL IS TOO LOW!\n"
          "Please turn ON the motor.",
          ""
      );

      lastWarning = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =====================================================
// HANDLE TELEGRAM MESSAGES
// =====================================================

void handleNewMessages(int numNewMessages)
{
  for (int i = 0; i < numNewMessages; i++)
  {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;

    // -------------------------------------------------
    // ACCESS CONTROL
    // -------------------------------------------------

    if (chat_id != OWNER_CHAT_ID)
    {
      bot.sendMessage(
          chat_id,
          "❌ Access Denied",
          ""
      );

      continue;
    }

    // -------------------------------------------------
    // /START
    // -------------------------------------------------

    if (text == "/start")
    {
      sendStartMessage(chat_id);
    }

    // -------------------------------------------------
    // /ON
    // -------------------------------------------------

    else if (text == "/on")
    {
      digitalWrite(MOTOR_PIN, HIGH);
      digitalWrite(ON_LED_PIN, HIGH);
      digitalWrite(OFF_LED_PIN, LOW);

      motorState = true;

      bot.sendMessage(
          chat_id,
          "✅ Transistor / Motor ON",
          ""
      );
    }

    // -------------------------------------------------
    // /OFF
    // -------------------------------------------------

    else if (text == "/off")
    {
      digitalWrite(MOTOR_PIN, LOW);
      digitalWrite(ON_LED_PIN, LOW);
      digitalWrite(OFF_LED_PIN, HIGH);

      motorState = false;

      bot.sendMessage(
          chat_id,
          "🛑 Transistor / Motor OFF",
          ""
      );
    }

    // -------------------------------------------------
    // /READINGS
    // -------------------------------------------------

    else if (text == "/readings")
    {
      sendReadings(chat_id);
    }

    // -------------------------------------------------
    // /TEMP
    // -------------------------------------------------

    else if (text == "/temp")
    {
      float temp;

      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
      {
        temp = sensorData.temperature;
        xSemaphoreGive(sensorMutex);
      }

      bot.sendMessage(
          chat_id,
          "🌡 Temperature: " +
          String(temp, 1) +
          " °C",
          ""
      );
    }

    // -------------------------------------------------
    // /HUMIDITY
    // -------------------------------------------------

    else if (text == "/humidity")
    {
      float hum;

      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
      {
        hum = sensorData.humidity;
        xSemaphoreGive(sensorMutex);
      }

      bot.sendMessage(
          chat_id,
          "💧 Humidity: " +
          String(hum, 1) +
          " %",
          ""
      );
    }

    // -------------------------------------------------
    // /SOIL
    // -------------------------------------------------

    else if (text == "/soil")
    {
      float soil;

      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
      {
        soil = sensorData.soilMoisture;
        xSemaphoreGive(sensorMutex);
      }

      bot.sendMessage(
          chat_id,
          "🌱 Soil Moisture: " +
          String(soil, 1) +
          "%",
          ""
      );
    }

    // -------------------------------------------------
    // /LIGHT
    // -------------------------------------------------

    else if (text == "/light")
    {
      float lux;

      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
      {
        lux = sensorData.lightIntensity;
        xSemaphoreGive(sensorMutex);
      }

      bot.sendMessage(
          chat_id,
          "☀️ Light Intensity: " +
          String(lux, 2) +
          " lux",
          ""
      );
    }

    // -------------------------------------------------
    // UNKNOWN COMMAND
    // -------------------------------------------------

    else
    {
      bot.sendMessage(
          chat_id,
          "❓ Unknown command.\n"
          "Use /start to see available commands.",
          ""
      );
    }
  }
}

// =====================================================
// SEND START MESSAGE
// =====================================================

void sendStartMessage(String chat_id)
{
  String welcome;

  welcome = "🌾 *Field Monitoring System*\n\n";

  welcome += "Available Commands:\n\n";

  welcome += "/readings - All sensor data\n";
  welcome += "/temp - Temperature\n";
  welcome += "/humidity - Humidity\n";
  welcome += "/soil - Soil moisture\n";
  welcome += "/light - Light intensity\n";
  welcome += "/on - Turn motor ON\n";
  welcome += "/off - Turn motor OFF\n";

  bot.sendMessage(
      chat_id,
      welcome,
      "Markdown"
  );
}

// =====================================================
// SEND ALL SENSOR READINGS
// =====================================================

void sendReadings(String chat_id)
{
  float temp;
  float hum;
  float soil;
  float lux;

  if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)))
  {
    temp = sensorData.temperature;
    hum = sensorData.humidity;
    soil = sensorData.soilMoisture;
    lux = sensorData.lightIntensity;

    xSemaphoreGive(sensorMutex);
  }
  else
  {
    bot.sendMessage(
        chat_id,
        "⚠️ Unable to read sensors.",
        ""
    );

    return;
  }

  String msg;

  msg = "📊 *Sensor Readings*\n\n";

  msg += "🌡 Temperature: ";
  msg += String(temp, 1);
  msg += " °C\n";

  msg += "💧 Humidity: ";
  msg += String(hum, 1);
  msg += " %\n";

  msg += "🌱 Soil Moisture: ";
  msg += String(soil, 1);
  msg += " %\n";

  msg += "☀️ Light Intensity: ";
  msg += String(lux, 2);
  msg += " lux\n";

  msg += "\n⚙️ Motor: ";

  if (motorState)
    msg += "ON";
  else
    msg += "OFF";

  bot.sendMessage(
      chat_id,
      msg,
      "Markdown"
  );
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  // ---------------------------------------------------
  // PIN MODES
  // ---------------------------------------------------

  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(ON_LED_PIN, OUTPUT);
  pinMode(OFF_LED_PIN, OUTPUT);

  digitalWrite(MOTOR_PIN, LOW);
  digitalWrite(ON_LED_PIN, LOW);
  digitalWrite(OFF_LED_PIN, HIGH);

  // ---------------------------------------------------
  // DHT
  // ---------------------------------------------------

  dht.begin();

  // ---------------------------------------------------
  // INITIAL SENSOR VALUES
  // ---------------------------------------------------

  sensorData.temperature = 0;
  sensorData.humidity = 0;
  sensorData.soilMoisture = 0;
  sensorData.lightIntensity = 0;

  // ---------------------------------------------------
  // CREATE MUTEX
  // ---------------------------------------------------

  sensorMutex = xSemaphoreCreateMutex();

  if (sensorMutex == NULL)
  {
    Serial.println("ERROR: Mutex creation failed!");
    while (1)
    {
      delay(1000);
    }
  }

  // ---------------------------------------------------
  // CONNECT WIFI
  // ---------------------------------------------------

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // ---------------------------------------------------
  // TELEGRAM SSL
  // ---------------------------------------------------

  client.setInsecure();

  // ---------------------------------------------------
  // CREATE FREERTOS TASKS
  // ---------------------------------------------------

  xTaskCreatePinnedToCore(
      sensorTask,          // Task function
      "SensorTask",        // Task name
      4096,                // Stack size
      NULL,                // Parameter
      2,                   // Priority
      &sensorTaskHandle,   // Task handle
      1                    // Core 1
  );

  xTaskCreatePinnedToCore(
      telegramTask,
      "TelegramTask",
      8192,
      NULL,
      1,
      &telegramTaskHandle,
      0                    // Core 0
  );

  xTaskCreatePinnedToCore(
      motorTask,
      "MotorTask",
      2048,
      NULL,
      3,                   // Higher priority
      &motorTaskHandle,
      1
  );

  Serial.println("FreeRTOS tasks started");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  // Nothing required here.
  // FreeRTOS tasks are running independently.

  vTaskDelay(pdMS_TO_TICKS(1000));
}
