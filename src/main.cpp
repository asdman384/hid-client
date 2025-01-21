#include <Arduino.h>
#include <NimBLEDevice.h>

// Joystick HID report format
typedef struct __attribute__((__packed__))
{
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t rz;
    uint8_t brake;
    uint8_t accel;
    uint8_t hat;
    uint16_t buttons;
} joystick_t;

static const char HID_SERVICE[] = "1812";
static const char HID_REPORT_MAP[] = "2A4B";
static const char HID_REPORT_DATA[] = "2A4D";

static const NimBLEAdvertisedDevice *advDevice;
static bool doConnect = false;
static uint32_t scanTimeMs = 5000; /** scan time in milliseconds, 0 = scan forever */
static bool deviceNewData = false;
static joystick_t Joystick_Report;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ClientCallbacks : public NimBLEClientCallbacks
{
    void onConnect(NimBLEClient *pClient) override
    {
        Serial.printf("Connected\n");
        /** After connection we should change the parameters if we don't need fast response times.
         *  These settings are 150ms interval, 0 latency, 450ms timout.
         *  Timeout should be a multiple of the interval, minimum is 100ms.
         *  I find a multiple of 3-5 * the interval works best for quick response/reconnect.
         *  Min interval: 120 * 1.25ms = 150, Max interval: 120 * 1.25ms = 150, 0 latency, 60 * 10ms = 600ms timeout
         */
        pClient->updateConnParams(120, 120, 0, 60);
    }

    void onDisconnect(NimBLEClient *pClient, int reason) override
    {
        Serial.printf("%s Disconnected, reason = %d - Starting scan\n", pClient->getPeerAddress().toString().c_str(), reason);
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }

    /** Called when the peripheral requests a change to the connection parameters.
     *  Return true to accept and apply them or false to reject and keep
     *  the currently used parameters. Default will return true.
     */
    bool onConnParamsUpdateRequest(NimBLEClient *pClient,
                                   const ble_gap_upd_params *params)
    {
        // Failing to accepts parameters may result in the remote device
        // disconnecting.
        return true;
    }

    /********************* Security handled here *********************/
    /****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest()
    {
        Serial.println("Client Passkey Request");
        /** return the passkey to send to the server */
        return 123456;
    }

    void onPassKeyEntry(NimBLEConnInfo &connInfo) override
    {
        Serial.printf("Server Passkey Entry\n");
        /**
         * This should prompt the user to enter the passkey displayed
         * on the peer device.
         */
        NimBLEDevice::injectPassKey(connInfo, 123456);
    }

    void onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t pass_key) override
    {
        Serial.printf("The passkey YES/NO number: %" PRIu32 "\n", pass_key);
        /** Inject false if passkeys don't match. */
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    bool onConfirmPIN(uint32_t pass_key)
    {
        Serial.print("The passkey YES/NO number: ");
        Serial.println(pass_key);
        /** Return false if passkeys don't match. */
        return true;
    }

    /** Pairing process complete, we can check the results in connInfo */
    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
    {
        if (!connInfo.isEncrypted())
        {
            Serial.printf("Encrypt connection failed - disconnecting\n");
            /** Find the client with the connection handle provided in connInfo */
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
            return;
        }
    }
} clientCallbacks;

/** Define a class to handle the callbacks when scan events are received */
class ScanCallbacks : public NimBLEScanCallbacks
{
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override
    {
        Serial.printf("Advertised Device found: %s\n", advertisedDevice->toString().c_str());
        if (advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE)))
        {
            Serial.printf("Found Our Service: %\n", advertisedDevice->toString().c_str());
            /** stop scan before connecting */
            NimBLEDevice::getScan()->stop();
            /** Save the device reference in a global for the client to use*/
            advDevice = advertisedDevice;
            /** Ready to connect now */
            doConnect = true;
        }
    }

    /** Callback to process the results of the completed scan or restart it */
    void onScanEnd(const NimBLEScanResults &results, int reason) override
    {
        Serial.printf("Scan Ended, reason: %d, device count: %d; Restarting scan\n", reason, results.getCount());
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }
} scanCallbacks;

/** Notification / Indication receiving handler callback */
// WARNING: This device has 4 Characteristics = 0x2a4d but with different
// handle values.
void notifyCB(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)
{
    std::string str = (isNotify == true) ? "Notification" : "Indication";
    str += " from ";
    str += pRemoteCharacteristic->getClient()->getPeerAddress().toString();
    str += ": Service = " + pRemoteCharacteristic->getRemoteService()->getUUID().toString();
    str += ", Characteristic = " + pRemoteCharacteristic->getUUID().toString();
    // str += ", Value = " + std::string((char *)pData, length);
    str += ", Value = ";
    for (size_t i = 0; i < length; i++)
    {
        uint8_t d = pData[i];
        str += std::to_string(d) + ", ";
        // Serial.print(d + ' ');
    }

    Serial.printf("%s\n", str.c_str());

    memcpy(&Joystick_Report, pData, sizeof(Joystick_Report));
    deviceNewData = true;
}

/** Handles the provisioning of clients and connects / interfaces with the server */
bool connectToServer()
{
    NimBLEClient *pClient = nullptr;

    /** Check if we have a client we should reuse first **/
    if (NimBLEDevice::getCreatedClientCount())
    {
        /**
         *  Special case when we already know this device, we send false as the
         *  second argument in connect() to prevent refreshing the service database.
         *  This saves considerable time and power.
         */
        pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if (pClient)
        {
            if (!pClient->connect(advDevice, false))
            {
                Serial.printf("Reconnect failed\n");
                return false;
            }
            Serial.printf("Reconnected client\n");
        }
        else
        {
            /**
             *  We don't already have a client that knows this device,
             *  check for a client that is disconnected that we can use.
             */
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }

    /** No client to reuse? Create a new one. */
    if (!pClient)
    {
        if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS)
        {
            Serial.printf("Max clients reached - no more connections available\n");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        Serial.printf("New client created\n");

        pClient->setClientCallbacks(&clientCallbacks, false);
        /**
         *  Set initial connection parameters:
         *  These settings are safe for 3 clients to connect reliably, can go faster if you have less
         *  connections. Timeout should be a multiple of the interval, minimum is 100ms.
         *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
         */
        pClient->setConnectionParams(12, 12, 0, 51);

        /** Set how long we are willing to wait for the connection to complete (milliseconds), default is 30000. */
        pClient->setConnectTimeout(5 * 1000);

        if (!pClient->connect(advDevice))
        {
            /** Created a client but failed to connect, don't need to keep it as it has no data */
            NimBLEDevice::deleteClient(pClient);
            Serial.printf("Failed to connect, deleted client\n");
            return false;
        }
    }

    if (!pClient->isConnected())
    {
        if (!pClient->connect(advDevice))
        {
            Serial.printf("Failed to connect\n");
            return false;
        }
    }

    Serial.printf("Connected to: %s RSSI: %d\n",
                  pClient->getPeerAddress().toString().c_str(),
                  pClient->getRssi());

    /** Now we can read/write/subscribe the characteristics of the services we are interested in */
    NimBLERemoteService *pSvc = nullptr;

    pSvc = pClient->getService(HID_SERVICE);
    /** make sure it's not null */
    if (pSvc)
    {
        // Subscribe to characteristics HID_REPORT_DATA.
        // One real device reports 2 with the same UUID but
        // different handles. Using getCharacteristic() results
        // in subscribing to only one.
        const std::vector<NimBLERemoteCharacteristic *> &charvector = pSvc->getCharacteristics(true);
        for (auto &it : charvector)
        {
            if (it->getUUID() == NimBLEUUID(HID_REPORT_DATA))
            {
                Serial.printf("Subscribe to characteristics HID_REPORT_DATA: %s  \n",
                              it->toString().c_str());

                if (it->canNotify())
                {
                    if (!it->subscribe(true, notifyCB))
                    {
                        /** Disconnect if subscribe failed */
                        Serial.println("subscribe notification failed");
                        pClient->disconnect();
                        return false;
                    }
                }
            }
        }
    }

    Serial.printf("Done with this device!\n");
    return true;
}

void setup()
{
    Serial.begin(115200);
    Serial.printf("Starting NimBLE Client\n");

    /** Initialize NimBLE, no device name spcified as we are not advertising */
    NimBLEDevice::init("");

    /**
     * Set the IO capabilities of the device, each option will trigger a different pairing method.
     *  BLE_HS_IO_KEYBOARD_ONLY   - Passkey pairing
     *  BLE_HS_IO_DISPLAY_YESNO   - Numeric comparison pairing
     *  BLE_HS_IO_NO_INPUT_OUTPUT - DEFAULT setting - just works pairing
     */
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY); // use passkey
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); //use numeric comparison

    /**
     * 2 different ways to set security - both calls achieve the same result.
     *  no bonding, no man in the middle protection, BLE secure connections.
     *  These are the default values, only shown here for demonstration.
     */
    NimBLEDevice::setSecurityAuth(true, false, true);

    // NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

    /** Optional: set the transmit power */
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */

    /** Optional: set any devices you don't want to get advertisments from */
    // NimBLEDevice::addIgnored(NimBLEAddress ("aa:bb:cc:dd:ee:ff"));

    /** create new scan */
    NimBLEScan *pScan = NimBLEDevice::getScan();

    /** Set the callbacks to call when scan events occur, no duplicates */
    pScan->setScanCallbacks(&scanCallbacks, false);

    /** Set scan interval (how often) and window (how long) in milliseconds */
    pScan->setInterval(100);
    pScan->setWindow(100);

    /**
     * Active scan will gather scan response data from advertisers
     *  but will use more energy from both devices
     */
    pScan->setActiveScan(true);

    /** Start scanning for advertisers */
    pScan->start(scanTimeMs);
    Serial.printf("Scanning for peripherals\n");
}

void loop()
{
    /** Loop here until we find a device we want to connect to */
    delay(10);

    if (doConnect)
    {
        doConnect = false;
        /** Found a device we want to connect to, do it now */
        if (connectToServer())
        {
            Serial.printf("Success! we should now be getting notifications, scanning for more!\n");
        }
        else
        {
            Serial.printf("Failed to connect, starting scan\n");
            NimBLEDevice::getScan()->start(scanTimeMs, false, true);
        }
    }

    if (deviceNewData)
    {
        static uint8_t last_x;
        static uint8_t last_y;
        // movement_callback_t f = this->get_movement_callback();
        // if (f)
        // {
        if ((last_x != Joystick_Report.x) || (last_y != Joystick_Report.y))
        {
            // (*f)(Joystick_Report.x, Joystick_Report.y);
            last_x = Joystick_Report.x;
            last_y = Joystick_Report.y;
            // Serial.printf("x=%d, y=%d\n", last_x, last_y);
        }
        // }

        static uint16_t last_buttons = 0;
        uint16_t changed = last_buttons ^ Joystick_Report.buttons;
        uint16_t buttons = Joystick_Report.buttons;
        for (size_t i = 0; i < 16; i++)
        {
            // button_callback_t f = this->get_button_callback(i);
            if ((changed & 1))
            {
                // (*f)(buttons & 1);
                // Serial.printf("button=%d\n", buttons);
            }
            changed >>= 1;
            buttons >>= 1;
        }
        last_buttons = Joystick_Report.buttons;

        deviceNewData = false;
    }
}