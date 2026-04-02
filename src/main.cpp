#include <Arduino.h>
#include <Preferences.h>  // The Preferences library is unique to Arduino-esp32
                          // It uses a portion of on-board non-volitile memory
                          // (NVS) of the ESP32 to store data.
                          // Preferences data is stored in NVS in sections
                          // called "namespace". There  are e set of key-value pars.
                          // LIke variables, a key-value pair has a data type.                          
#include <WiFi.h>
#include <WebServer.h>    // Library for the HTTP server
#include <string>         // C++ Standard String library
#include <string_view>    // C++17 header for high-performance string handling
                          // Just to data members: a pointer and a length. 
                          // Does not created a copy in memory. Read-only.

// --- Modern C++: Namespaces & Constexpr ---
// We use a namespace to group related constants. This prevents "LED_PIN" from 
// accidentally conflicting with other libraries.
namespace Config {
    // 'constexpr' tells the compiler this value is known at compile-time.
    // It is more efficient than 'const' and safer than '#define'.
    constexpr int ButtonPin = 47;
    constexpr int LedPin    = 2;
    constexpr int StrobPin  = 48;
    constexpr int PanicPin  = 45;

    // WiFi Credentials using std::string_view
    // This is C++17's way to point to a string without copying it into memory.

    // ASI Network
    //constexpr std::string_view Ssid     = "ASI Personal";
    //constexpr std::string_view Password = "Personal@AcceleratedSystems";

    // RPi Network (192.168.4.11)
    constexpr std::string_view Ssid     = "ESP32test-network";
    constexpr std::string_view Password = "esp32test123";
}

// Global Objects
constexpr bool RW_MODE = false;
constexpr bool RO_MODE = true;
Preferences prefs;          // Created name of the Preferences object. This object is 
                            // used with the Preferences methods to access the
                            // name space and the key-value pairs it contains.
                                    
SemaphoreHandle_t panicSemaphore;
WebServer server(80);       // Create a server on port 80 (standard HTTP)

// Prototypes
void IRAM_ATTR handleButtonInterrupt();
void panicTask(void* pvParameters);
void heartbeatTask(void* pvParameters);
void serverTask(void* pvParameters);    // Task to handle incoming web requests
void initWiFi();
void handleRoot();                      // Function that serves the HTML page
void handleReset();                     // New function to clear NVS
// Helper function to determine signal color based on RSSI
std::string_view getSignalColor(int rssi);  

void setup() {
    delay(1000);
    Serial.begin(115200);
    delay(5000);

    Serial.println("\n--- Connected via CH343 UART (COM?) ---");
    Serial.println(  "--- ESP32-S3 Dual Core Booting --------");
    Serial.println("\n--- ESP Hardware Info------------------");
    
    // Display ESP Information
    Serial.printf("Chip ID: %u\n", ESP.getChipModel()); // Get the 24-bit chip ID
    Serial.printf("CPU Frequency: %u MHz\n", ESP.getCpuFreqMHz()); // Get CPU frequency
    Serial.printf("SDK Version: %s\n", ESP.getSdkVersion()); // Get SDK version

    // Get and print flash memory information
    Serial.printf("Flash Chip Size: %u bytes\n", ESP.getFlashChipSize()); // Get total flash size
   
    // Get and print SRAM memory information
    Serial.printf("Internal free Heap at setup: %d bytes\n", ESP.getFreeHeap());
    if(psramFound()){
        Serial.printf("Total PSRAM Size: %d bytes", ESP.getPsramSize());
    } else {
         Serial.print("No PSRAM found");
    }
    Serial.println("\n---------------------------------------");
    Serial.print("\n");  


    // Initialise WiFi early in the boot sequence
    initWiFi();

    // Setup Web Server Routes
    server.on("/", handleRoot);         // When someone visits http://192.168.4.11/, call handleRoot
    server.on("/reset", handleReset);   // Route for the reset button
    server.begin();
    Serial.println("HTTP Server Started.");

    // Initialise NVS and read lifetime count
    prefs.begin("system", RO_MODE);     // Open namespace "system" and make it available
                                        // in READ ONLY (RO) mode.

    // 'auto' - C++17 type deduction (compiler knows this is a uint32_t)
    auto totalPanics = prefs.getUInt("panic_count", 0);   // Retrive value of the "panic_count" 
                                                          // key, define to 0 if not found
    Serial.printf("Bootup - Lifetime Panic Events: %u\n", totalPanics);
    Serial.print("\n");
    prefs.end(); // Close our preference namespace.

    // Create binary semaphore
    panicSemaphore = xSemaphoreCreateBinary();

    // Configure Hardware using our Namespace
    pinMode(Config::ButtonPin, INPUT_PULLUP);
    pinMode(Config::StrobPin,  OUTPUT);
    pinMode(Config::PanicPin,  OUTPUT);
    pinMode(Config::LedPin,    OUTPUT);

    attachInterrupt(digitalPinToInterrupt(Config::ButtonPin), handleButtonInterrupt, FALLING);

    // --- Task Distribution ---
    // Panic Task: Core 1 (High Priority)
    xTaskCreatePinnedToCore(panicTask, "Panic", 4096, NULL, 3, NULL, 1);

    // Heartbeat: Core 0 (Low Priority)
    xTaskCreatePinnedToCore(heartbeatTask, "Heartbeat", 4096, NULL, 1, NULL, 0);

    // Web Server: Core 0 (Medium Priority)
    // Running network tasks on Core 0 is standard as the WiFi stack lives there.
    xTaskCreatePinnedToCore(serverTask, "WebServer", 4096, NULL, 2, NULL, 0);

    // Terminate the initial Arduino setup task to free up memory
    vTaskDelete(NULL);
}

// The Linker needs this function to exist, even if it is never called.
void loop() {}

// Helper function to determine signal color based on RSSI
std::string_view getSignalColor(int rssi) {
    if (rssi > -60) return "#4caf50"; // Green (Strong)
    if (rssi > -80) return "#ff9800"; // Orange (Okay)
    return "#f44336";                // Red (Weak)
}

// --- New Function: Handle Reset ---
void handleReset() {
    Serial.println("[Web] Reset request received. Clearing NVS...");
    prefs.begin("system", false); // Open in R/W mode
    prefs.clear();               // Removes all keys in the "system" namespace
    prefs.end();
    
    // Redirect the browser back to the root page after resetting
    server.sendHeader("Location", "/");
    server.send(303); 
}

// --- Updated handleRoot with CSS Button ---
void handleRoot() {
    prefs.begin("system", true);
    auto count = prefs.getUInt("panic_count", 0);
    prefs.end();

    int rssi = WiFi.RSSI();
    auto signalColor = getSignalColor(rssi);

    // Using C++ Raw String Literal for the UI
    std::string html = R"=====(
    <!DOCTYPE html>
    <html>
    <head>
        <meta name='viewport' content='width=device-width, initial-scale=1.0'>
        <meta http-equiv='refresh' content='5'>
        <title>S3 Dashboard</title>
        <style>
            body { font-family: 'Segoe UI', sans-serif; background-color: #121212; color: #e0e0e0; margin: 0; display: flex; justify-content: center; align-items: center; min-height: 100vh; }
            .container { width: 90%; max-width: 400px; text-align: center; }
            .card { background: #1e1e1e; border-radius: 12px; padding: 24px; margin-bottom: 20px; box-shadow: 0 8px 16px rgba(0,0,0,0.4); border-top: 4px solid #00adb5; }
            h1 { color: #00adb5; font-size: 1.2rem; letter-spacing: 2px; margin-bottom: 30px; }
            .label { font-size: 0.8rem; color: #757575; text-transform: uppercase; }
            .value { font-size: 3.5rem; font-weight: bold; margin: 10px 0; color: #ffffff; }
            .btn-reset { display: inline-block; background-color: #ff4b2b; color: white; padding: 12px 24px; text-decoration: none; border-radius: 8px; font-weight: bold; margin-top: 10px; transition: 0.3s; }
            .btn-reset:hover { background-color: #ff1f00; scale: 1.05; }
            .footer { font-size: 0.7rem; color: #f9f970; margin-top: 20px; }
        </style>
    </head>
    <body>
        <div class='container'>
            <h1>CORE MONITOR v1.0</h1>
            
            <div class='card' style='border-top-color: #ff4b2b;'>
                <div class='label'>Panic Events</div>
                <div class='value'>)=====";
    
    html += std::to_string(count);
    
    html += R"=====(</div>
                <a href='/reset' class='btn-reset' onclick="return confirm('Clear lifetime logs?')">RESET COUNTER</a>
            </div>

            <div class='card' style='border-top-color: )=====";
    html += signalColor;
    html += R"=====(;'>
                <div class='label'>RSSI Strength</div>
                <div class='value' style='font-size: 2.5rem;'>)=====";
    html += std::to_string(rssi);
    html += R"=====( <span style='font-size: 1rem;'>dBm</span></div>
            </div>
            
            <div class='footer'>ESP32-S3 | Uptime: )=====";
    html += std::to_string(millis() / 1000);
    html += R"=====(s</div>
        </div>
    </body>
    </html>
    )=====";

    server.send(200, "text/html", html.c_str());
}

// --- Web Server Task ---
void serverTask(void* pvParameters) {
    for(;;) {
        server.handleClient(); // Check for new clients
        vTaskDelay(pdMS_TO_TICKS(5)); // Yield to keep Core 0 smooth
    }
}

// --- WiFi Initialization ---
void initWiFi() {
    // WiFi Status 
    //Serial.print(" Initial WiFi status - ");
    //Serial.println(WiFi.status());

        // Scan WiFi network 
    Serial.println("\n--- WiFi Network Info------------------");

    // Set WiFi to station mode and disconnect from an AP 
    // if it was previously connected
    WiFi.mode(WIFI_STA);
    //WiFi.disconnect();
    delay(500);
    //Serial.print(" WiFi status befor scanning - ");
    //Serial.println(WiFi.status());

    auto n = WiFi.scanNetworks();
    Serial.println("   ... Starting WiFi Scan ...");
    if (n ==0) {
        Serial.println("No networks found");
    } else {
        Serial.print(n);
        Serial.println(" - networks found");

        for (int i = 0; i < n; ++i) {
            // Print SSID and RSSI for each network found
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.print(WiFi.SSID(i));
            Serial.print(" (");
            Serial.print(WiFi.RSSI(i));
            Serial.print(")");
            Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?" ":"*");
        delay(500);
        }
    }
    Serial.print("\n"); 
    Serial.print("    WiFi Scan Completed");
    Serial.println("\n---------------------------------------");
    Serial.print("\n"); 

    Serial.println("Connecting to WiFi (monitoring status)...");
    // .data() converts string_view back to the char* that WiFi.begin needs
    // .data() provides the raw pointer WiFi.begin() requires   
    WiFi.begin(Config::Ssid.data(), Config::Password.data());

    // Simple connection loop
    // status = 0 (WL_IDLE_STATUS): temporary status assigned when WiF.begin() is called
    // status = 1 (WL_NO_SSID_AVAIL): whem no SSID are available
    // status = 2 (WL_SCAN_COMPLETED): scan networks is completed
    // status = 3 (WL_CONNECTED): when connected to a WiFi network
    // status = 4 (WL_CONNECT_FAILDE): when connection fails for all the attempts
    // status = 5 (WL_CONNECTION_LOST): when the connection is lost
    // status = 6 (WL_DISCONNECTED): when disconnected from a network

    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
        Serial.print(WiFi.status());
    }
    Serial.print("\n");
    Serial.print("\nCONNECTED!");
    Serial.print("  RSSI: ");
    Serial.println(WiFi.RSSI());  // WiFi connection strength
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("\n");

    // Contimue monitoring WiFi status
    ///for(;;){
    //    delay(100);
    //    Serial.print(WiFi.status());
    //}
}

// --- Interrupt Service Routine ---
void IRAM_ATTR handleButtonInterrupt() {
    digitalWrite(Config::StrobPin, HIGH);

    // static ensures this variable is created once, not every time ISR runs
    static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(panicSemaphore, &xHigherPriorityTaskWoken);
    
    digitalWrite(Config::StrobPin, LOW);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// --- High Priority Panic Handler ---
void panicTask(void* pvParameters) {
    for(;;) {
        // Task blocks here (0% CPU) waiting for the semaphore
        // portMAX_DELAY means the task sleeps until the semaphore is "given"
        if (xSemaphoreTake(panicSemaphore, portMAX_DELAY) == pdPASS) {
            detachInterrupt(digitalPinToInterrupt(Config::ButtonPin));
            digitalWrite(Config::PanicPin, HIGH);

             // Update (increment) NVS Persistence
            prefs.begin("system", RW_MODE);
            auto count = prefs.getUInt("panic_count", 0) + 1;
            prefs.putUInt("panic_count", count);
            prefs.end();

            Serial.printf("[Panic] Event #%u stored. WiFi Status: %d\n", count, WiFi.status());

             // Visual strobe feedback
            for(int i = 0; i < 20; ++i) {
                digitalWrite(Config::LedPin, !digitalRead(Config::LedPin));
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            digitalWrite(Config::PanicPin, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));  // Cool-down delay
            
            // C++17 "Flush": Empty the semaphore of any extra signals from bouncing
            // C++17 style: we can use 'while' to clear any pending semaphores (bounces)
            while(xSemaphoreTake(panicSemaphore, 0) == pdPASS); 
            
            // Re-arm the hardware
            attachInterrupt(digitalPinToInterrupt(Config::ButtonPin), handleButtonInterrupt, FALLING);
        }
    }
}

void heartbeatTask(void* pvParameters) {
    for(;;) {
        digitalWrite(Config::LedPin, !digitalRead(Config::LedPin));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}