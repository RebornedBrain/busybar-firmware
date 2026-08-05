#pragma once

#include "ble_intercom_types.h"
#include "ble_system_command.h"

/**
 * @brief Opaque command engine handle
 */
typedef struct BleCommandEngine BleCommandEngine;

/**
 * @brief Allocates command engine instance with command array provided from the outside
 *
 * @param[in] ble Pointer to ble instance
 * @param[in] commands Predefined command array
 * @param[in] commands_count Amount of commands in array
 * @param[in] extract_frame Frame extraction callback, separate for each chip
 * @param[out] BleCommandEngine* Pointer to command engine instance
 */
BleCommandEngine* ble_command_engine_alloc(
    Ble* ble,
    const BleCommandItem* commands,
    uint8_t commands_count,
    FuriEventLoop* event_loop);

/**
 * @brief Perform command processing, command frame can be extracted from command 
 * buffer or from intercom for U5 and only from intercom for 917
 *
 * @param[in] instance Pointer to engine instance
 * @param[in] source Source from where frame should be taken
 * @param[out] true when command was processed successfully, otherwise false
 */
bool ble_command_engine_run(BleCommandEngine* instance, BleCommandEngineExtractFrameSource source);
