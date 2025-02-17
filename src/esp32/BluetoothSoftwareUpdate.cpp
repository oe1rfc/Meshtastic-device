#include <Arduino.h>

#include "../concurrency/LockGuard.h"
#include "../graphics/Screen.h"
#include "../main.h"
#include "BluetoothSoftwareUpdate.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioLibInterface.h"
#include "configuration.h"
#include "nimble/BluetoothUtil.h"

#include <CRC32.h>
#include <Update.h>

int16_t updateResultHandle = -1;

static CRC32 crc;

static uint32_t updateExpectedSize, updateActualSize;
static uint8_t update_result;
static uint8_t update_region;

static concurrency::Lock *updateLock;

/// Handle writes & reads to total size
int update_size_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    concurrency::LockGuard g(updateLock);

    // Check if there is enough to OTA Update
    chr_readwrite32le(&updateExpectedSize, ctxt);

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && updateExpectedSize != 0) {
        updateActualSize = 0;
        crc.reset();
        if (Update.isRunning())
            Update.abort();
        bool canBegin = Update.begin(updateExpectedSize, update_region);
        DEBUG_MSG("Setting region %d update size %u, result %d\n", update_region, updateExpectedSize, canBegin);
        if (!canBegin) {
            // Indicate failure by forcing the size to 0 (client will read it back)
            updateExpectedSize = 0;
        } else {
            // This totally breaks abstraction to up up into the app layer for this, but quick hack to make sure we only
            // talk to one service during the sw update.
            // DEBUG_MSG("FIXME, crufty shutdown of mesh bluetooth for sw update.");
            // void stopMeshBluetoothService();
            // stopMeshBluetoothService();

            screen->startFirmwareUpdateScreen();
            if (RadioLibInterface::instance)
                RadioLibInterface::instance->disable(); // FIXME, nasty hack - the RF95 ISR/SPI code on ESP32 can fail while we
                                                        // are writing flash - shut the radio off during updates
        }
    }

    return 0;
}

#define MAX_BLOCKSIZE_FOR_BT 512

/// Handle writes to data
int update_data_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    concurrency::LockGuard g(updateLock);

    static uint8_t
        data[MAX_BLOCKSIZE_FOR_BT]; // we temporarily copy here because I'm worried that a fast sender might be able overwrite srcbuf

    uint16_t len = 0;

    auto rc = ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &len);
    assert(rc == 0);

    // DEBUG_MSG("Writing %u\n", len);
    crc.update(data, len);
    Update.write(data, len);
    updateActualSize += len;
    powerFSM.trigger(EVENT_FIRMWARE_UPDATE);

    return 0;
}

/// Handle writes to crc32
int update_crc32_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    concurrency::LockGuard g(updateLock);
    uint32_t expectedCRC = 0;
    chr_readwrite32le(&expectedCRC, ctxt);

    uint32_t actualCRC = crc.finalize();
    DEBUG_MSG("expected CRC %u\n", expectedCRC);

    uint8_t result = 0xff;

    if (updateActualSize != updateExpectedSize) {
        DEBUG_MSG("Expected %u bytes, but received %u bytes!\n", updateExpectedSize, updateActualSize);
        result = 0xe1;                   // FIXME, use real error codes
    } else if (actualCRC != expectedCRC) // Check the CRC before asking the update to happen.
    {
        DEBUG_MSG("Invalid CRC! expected=%u, actual=%u\n", expectedCRC, actualCRC);
        result = 0xe0; // FIXME, use real error codes
    } else {
        if (Update.end()) {
            if (update_region == U_SPIFFS) {
                DEBUG_MSG("Filesystem updated!\n");
                nodeDB.saveToDisk(); // Since we just wiped the filesystem, we need to save our current state
            } else {
                DEBUG_MSG("Appload updated, rebooting in 5 seconds!\n");
                rebootAtMsec = millis() + 5000;
            }
        } else {
            DEBUG_MSG("Error Occurred. Error #: %d\n", Update.getError());
        }
        result = Update.getError();
    }

    if (RadioLibInterface::instance)
        RadioLibInterface::instance->startReceive(); // Resume radio

    assert(updateResultHandle >= 0);
    update_result = result;
    DEBUG_MSG("BLE notify update result\n");
    auto res = ble_gattc_notify(curConnectionHandle, updateResultHandle);
    assert(res == 0);

    return 0;
}

int update_result_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return chr_readwrite8(&update_result, sizeof(update_result), ctxt);
}

int update_region_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return chr_readwrite8(&update_region, sizeof(update_region), ctxt);
}

/*
See bluetooth-api.md

 */
void reinitUpdateService()
{
    if (!updateLock)
        updateLock = new concurrency::Lock();

    auto res = ble_gatts_count_cfg(gatt_update_svcs); // assigns handles?  see docstring for note about clearing the handle list
                                                      // before calling SLEEP SUPPORT
    assert(res == 0);

    res = ble_gatts_add_svcs(gatt_update_svcs);
    assert(res == 0);
}
