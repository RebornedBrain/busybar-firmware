#include "ble_command_engine.h"
#include "ble_log.h"

#define TAG "BleCmdEng"

#include <api_lock.h>

typedef struct {
    BleSystemCommand command_code;
    FuriApiLock lock;
    size_t data_size;
    void* data;
    bool result;
    bool internal;
} BleCommand;

typedef struct {
    BleIntercomFrameHeader header;
    uint8_t data[];
} BleCommandIntercomFrame;

struct BleCommandEngine {
    uint8_t commands_count;
    const BleCommandItem* commands;
    Ble* ble;

    FuriMessageQueue* command_queue;
    FuriMutex* current_command_lock;
    BleCommand* current_command;

    BleCommandIntercomFrame* frame;
    size_t frame_size;
};

///TODO: Here we should make some factory which
//will return certain type of engine depending on System/Service parameter
//In that case we can do extern command lists here and not in upper layers
BleCommandEngine* ble_command_engine_alloc(
    Ble* ble,
    const BleCommandItem* commands,
    uint8_t commands_count,
    FuriEventLoop* event_loop) {
    furi_assert(ble);
    furi_assert(commands);
    furi_assert(commands_count > 0);

    BleCommandEngine* instance = malloc(sizeof(BleCommandEngine));
    instance->ble = ble;

    instance->commands = commands;
    instance->commands_count = commands_count;

    instance->current_command_lock = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->current_command = NULL;
    instance->command_queue = furi_message_queue_alloc(10, sizeof(BleCommand*));

    furi_event_loop_subscribe_message_queue(
        event_loop,
        instance->command_queue,
        FuriEventLoopEventIn,
        ble_command_engine_queue_handler,
        instance);

    return instance;
}

bool ble_command_engine_run(BleCommandEngine* instance, BleIntercomFrameGeneric* frame) {
    furi_assert(instance);
    furi_assert(frame);

    const BleIntercomFrameType frame_type = frame->header.frame_type;
    const BleCommandCode command = (BleCommandCode)frame->header.command;
    const BleCommandCode unknown_command = 0;
    furi_check(command != unknown_command);
    furi_check(command < instance->commands_count);
    const BleCommandItem* const item = &instance->commands[command];

    bool result = false;

    if(frame_type == BleIntercomFrameTypeRequest && item->request) {
        result = item->request(frame, instance->ble);
    } else if(frame_type == BleIntercomFrameTypeResponse && item->response) {
        result = item->response(frame, instance->ble);
    } else {
        furi_crash("Unknown frame");
    }

    return result;
}

static inline void
    ble_command_frame_check_alloc(BleCommandEngine* instance, const size_t new_frame_size) {
    if(new_frame_size > instance->frame_size) {
        instance->frame = realloc(instance->frame, new_frame_size);
        furi_check(instance->frame);
        instance->frame_size = new_frame_size;
    }
}

static void ble_command_convert_to_intercom_frame(
    BleCommandEngine* instance,
    const BleCommand* const command) {
    const size_t new_msg_size = sizeof(BleIntercomFrameHeader) + command->data_size + sizeof(bool);
    ble_command_frame_check_alloc(instance, new_msg_size);

    BleCommandIntercomFrame* frame = instance->frame;
    frame->header.command = command->command_code;
    frame->header.frame_type = BleIntercomFrameTypeRequest;
    frame->header.source = BleIntercomFrameSourceSystem;
    frame->header.data_size = command->data_size;

    if(command->data_size > 0) {
        memcpy(frame->data, command->data, command->data_size);
    }
}

