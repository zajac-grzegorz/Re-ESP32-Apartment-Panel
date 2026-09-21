#include <Arduino.h>
#include <Matter.h>
#include <MycilaESPConnect.h>
#include <PsychicMqttClient.h>
#include "ReCommon.h"
#include "ReLED.h"
#include "ReServer.h"

#include <esp_display_panel.hpp>
#include <esp_err.h>

#include <lvgl.h>

#include "esp_lv_adapter_arduino.h"
#include "ui.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static PsychicMqttClient mqttClient;
static MatterOnOffPlugin onOffPlugin;

ReServer* server = nullptr;
Mycila::ESPConnect* espConnect = nullptr;

// Matter protocol Endpoint Callback
bool setPluginOnOff(bool state) {
    logger.info(RE_TAG, "User Callback :: New Plugin State = %s", state ? "ON" : "OFF");
  
    if (false == state)
    {
        return true;
    }

    return true;
}

void setupMqttClient()
{
    if (false == config.get<bool>("mqtt_en"))
    {
        return;
    }

    std::string mqttIp = config.getString("mqtt_ip");
    std::string mqttServer = "mqtt://" + mqttIp + ":" + std::to_string(config.get<int>("mqtt_port"));
    mqttClient.setServer(mqttServer.c_str());
    mqttClient.setCredentials(config.getString("mqtt_user"), config.getString("mqtt_pass"));
    mqttClient.setClientId("RePanel");
    mqttClient.setCleanSession(false);
    mqttClient.setKeepAlive(60);
    mqttClient.setWill("repanel/status", 1, true, "RePanel OFFLINE");

    mqttClient.onTopic("repanel/msg", 2, [&](const char *topic, const char *payload, int retain, int qos, bool dup)
        {
            logger.debug(RE_TAG, "Received Topic: %s", topic);
            logger.debug(RE_TAG, "Received Payload: %s", payload);

            // if (!strcmp(payload, BOT_PRESS_COMMAND) || !strcmp(payload, BOT_STATUS_COMMAND))
            // {
            //     executeBotCommand(payload);
            // }
            // else
            // {
            //     logger.warn(RE_TAG, "Unknown command received over MQTT: %s", payload);
            //     mqttClient.publish("repanel/result", 1, true, "ERUnknown command received over MQTT"); 
            // }
        });

    mqttClient.onConnect([&](bool sessionPresent)
        {
            logger.debug(RE_TAG, "MQTT connected: %s, sessionPresent: %d", mqttClient.connected() ? "YES" : "NO", sessionPresent);
            logger.debug(RE_TAG, "MQTT clientID: %s", mqttClient.getClientId());
            
            mqttClient.publish("repanel/status", 1, true, "RePanel: ONLINE"); 
        });

    mqttClient.connect();
}

void setup()
{
    Serial.begin(115200);
    
    logger.forwardTo(&Serial);
    logger.debug(RE_TAG, "Using Serial as terminal");

        // Configure RGB led or normal led, depending on the board type
#ifdef PIN_RGB_LED
    ReLED.begin(PIN_RGB_LED, true, 300);
#else

#ifdef USB_PRODUCT 
    if (String(USB_PRODUCT) == "NanoC6")
    {
        pinMode(RGB_LED_PWR_PIN, OUTPUT);
        digitalWrite(RGB_LED_PWR_PIN, HIGH);
        ReLED.begin(RGB_LED_DATA_PIN, true, 300);
    }
    else
    {
        ReLED.begin(LED_BUILTIN, false, 300);
    }
#else
    ReLED.begin(LED_BUILTIN, false, 300);
#endif

#endif

    LED_STATUS_UPDATE(start(LED_WIFI_NEEDED));

    logger.debug(RE_TAG, "Starting Re ESP32 Apartment Panel");

    // Load configuration data from NVS
    configureStorage();

    // Setup the Async Web Server and ESPConnect for network management
    // do not change to order of these, as the server needs to be initialized before ESPConnect 
    // can use it for captive portal and config, and ESPConnect needs to be initialized before the 
    // server can use it for network state listening
    server = new ReServer(config.get<int>("dev_port"));

    espConnect = new Mycila::ESPConnect(*server);

    server->setESPConnect(espConnect);
    
    // Network state listener
    espConnect->listen([&](__unused Mycila::ESPConnect::State previous, __unused Mycila::ESPConnect::State state) 
    {
        switch (state)
        {
            case Mycila::ESPConnect::State::NETWORK_CONNECTING:
            case Mycila::ESPConnect::State::NETWORK_RECONNECTING:
                LED_STATUS_UPDATE(start(LED_WIFI_CONNECTING));
                break;
            case Mycila::ESPConnect::State::NETWORK_DISCONNECTED:
            case Mycila::ESPConnect::State::NETWORK_TIMEOUT:
                LED_STATUS_UPDATE(start(LED_ALERT));
                break;
            case Mycila::ESPConnect::State::NETWORK_CONNECTED:
                LED_STATUS_UPDATE(on());
                break;
            case Mycila::ESPConnect::State::PORTAL_COMPLETE: {
                logger.debug(RE_TAG, "Captive Portal has ended, save the configuration...");
                
                espConnect->saveConfiguration();

                config.setString("net_ssid", espConnect->getConfig().wifiSSID.c_str());
                config.setString("net_pass", espConnect->getConfig().wifiPassword.c_str());
                break;
            }
            default:
                break;
        }

        JsonDocument doc;
        espConnect->toJson(doc.to<JsonObject>());
        serializeJsonPretty(doc, Serial);
    });

    Mycila::ESPConnect::Config espConnectConfig;
    espConnect->loadConfiguration(espConnectConfig);
    espConnectConfig.hostname = "RePanel";
    espConnectConfig.wifiSSID = config.getString("net_ssid");
    espConnectConfig.wifiPassword = config.getString("net_pass");

    // Setup and start ESPConnect with the configuration loaded from NVS, or start captive portal if no config or connection fails
    espConnect->setAutoRestart(true);
    espConnect->setConnectTimeout(300);
    espConnect->setBlocking(true);
    logger.debug(RE_TAG, "Trying to connect to saved WiFi or will start portal...");
    espConnect->begin("RePanel", "", espConnectConfig);
    logger.debug(RE_TAG, "ESPConnect completed, continuing setup()...");

    // Start the Async Web Server
    server->begin();
    logger.debug(RE_TAG, "Async Web Server started");

    // To allow log viewing over the web
    configureWebSerial(config.get<bool>("adm_webserial"), server);

    Board *board = new Board();
    if ((board == nullptr) || !board->init())
    {
        Serial.println("Board init failed");
        while (true)
        {
            delay(1000);
        }
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    LCD *lcd = board->getLCD();
    if (lcd == nullptr)
    {
        Serial.println("LCD device is not available");
        while (true)
        {
            delay(1000);
        }
    }
    auto *lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB)
    {
        lcd->configFrameBufferNumber(frame_buffer_count);
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }

    assert(board->begin());

    board->getBacklight()->setBrightness(10);

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.task_priority = 2;
    adapter_config.task_core_id = ARDUINO_RUNNING_CORE;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    const uint16_t frame_width = static_cast<uint16_t>(lcd->getFrameWidth());
    const uint16_t frame_height = static_cast<uint16_t>(lcd->getFrameHeight());
    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        lcd, frame_width, frame_height, rotation);
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != nullptr);

    if (board->getTouch() != nullptr)
    {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, board->getTouch());
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != nullptr);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    // lv_demo_widgets();
    ui_init();

    esp_lv_adapter_unlock();

    Serial.println("LVGL v9.5.0 porting example ready");

    Serial.printf("Total heap: %d bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());

    if (config.get<bool>("dev_matter"))
    {
        logger.debug(RE_TAG, "Initializing Matter On/Off Plugin EndPoint");

        // Start the Matter On/Off Plugin EndPoint and set the user callback for when the state is changed by the Matter Controller
        onOffPlugin.begin();
        onOffPlugin.onChange(setPluginOnOff);

        // Matter beginning - Last step, after all EndPoints are initialized
        Matter.begin();

        // This may be a restart of a already commissioned Matter accessory
        if (Matter.isDeviceCommissioned()) 
        {
            logger.debug(RE_TAG, "Matter Node is commissioned and connected to the network. Ready for use");
            logger.debug(RE_TAG, "Initial state: %s", onOffPlugin.getOnOff() ? "ON" : "OFF");
            onOffPlugin.updateAccessory();  // configure the Plugin based on initial state
        }
        else
        {
            logger.debug(RE_TAG, "Matter comission code is: %s", Matter.getManualPairingCode().c_str());
        }
    }

    // If MQTT is enabled in config, setup the MQTT client and connect to the broker
    setupMqttClient();

    // Update the LED to indicate we are ready and waiting for BLE connection and commands
    LED_COLOR_UPDATE(LED_COLOR_GREEN);
}

void loop()
{
    // delay(100);

    espConnect->loop();
    ReLED.getStatusLED()->check();
}