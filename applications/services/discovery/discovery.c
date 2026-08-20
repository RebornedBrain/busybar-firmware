#include "discovery.h"

#include <m-array.h>

#include <lwip/api.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/apps/mdns.h>

#include <device_name/device_name.h>
#include <network/network.h>
#include <wifi/wifi.h>
#include <usb_network/usb_network.h>

#include <furi_hal_version.h>

#define TAG "Discovery"

// =====
// Types
// =====

typedef struct {
    struct netif* netif;
} DiscoveryInterface;

typedef struct {
    const DiscoveryServiceInfo* info;
    void* context;
} DiscoveryService;

ARRAY_DEF(DiscoveryServices, DiscoveryService, M_POD_OPLIST)
#define M_OPL_DiscoveryServices_t() ARRAY_OPLIST(DiscoveryServices, M_POD_OPLIST)

typedef enum {
    DiscoveryApiMessageTypeAddService,
    DiscoveryApiMessageTypeDeviceName,
    DiscoveryApiMessageTypeUsbNetwork,
    DiscoveryApiMessageTypeWifiNetwork,
    DiscoveryApiMessageTypeMax,
} DiscoveryApiMessageType;

typedef struct {
    DiscoveryApiMessageType type;
    union {
        DiscoveryService service_to_add;
        DeviceNameState device_name_state;
        UsbNetworkState usb_network_state;
        WifiState wifi_network_state;
    };
} DiscoveryApiMessage;

struct Discovery {
    FuriEventLoop* event_loop;
    FuriMessageQueue* api_queue;
    FuriString* device_name;
    DiscoveryInterface interfaces[NetworkNetifCount];
    DiscoveryServices_t services;

    DiscoveryServiceInfo device_discovery;
    char device_service_name[(FURI_HAL_VERSION_MAC_LENGTH * 2) + 1];

    WifiState wifi_state;
};

// ==============
// Internal logic
// ==============

static void
    discovery_send_api_message(Discovery* discovery, const DiscoveryApiMessage* api_message) {
    const FuriStatus status = furi_message_queue_put(discovery->api_queue, api_message, 1000);

    if(status != FuriStatusOk) {
        furi_check(status == FuriStatusErrorTimeout);
        FURI_LOG_W(TAG, "Api message queue overflow");
    }
}

static enum mdns_sd_proto discovery_transport_to_lwip(DiscoveryTransportType transport) {
    if(transport == DiscoveryTransportTypeTcp) {
        return DNSSD_PROTO_TCP;
    } else if(transport == DiscoveryTransportTypeUdp) {
        return DNSSD_PROTO_UDP;
    } else {
        furi_crash();
    }
}

static const char* discovery_device_name_to_hostname(const char* dev_name, FuriString* buffer) {
    furi_assert(buffer);

    furi_string_reset(buffer);

    char c;
    while((c = *(dev_name++))) {
        if(isalnum(c)) {
            furi_string_push_back(buffer, tolower(c));
        }
    }

    if(furi_string_empty(buffer)) {
        if(strcmp(dev_name, DEVICE_NAME_DEFAULT) == 0) {
            furi_crash("Default device name has no alphanumeric characters");
        }
        return discovery_device_name_to_hostname(DEVICE_NAME_DEFAULT, buffer);
    }

    return furi_string_get_cstr(buffer);
}

static void discovery_txt_adapter(struct mdns_service* lwip_srv, void* context) {
    furi_assert(lwip_srv);
    furi_assert(context);
    LWIP_ASSERT_CORE_LOCKED();

    DiscoveryService* service = context;
    const DiscoveryServiceInfo* info = service->info;

    if(info->txt_callback) {
        FuriString* txt = furi_string_alloc();

        info->txt_callback(txt, service->context);
        mdns_resp_add_service_txtitem(lwip_srv, furi_string_get_cstr(txt), furi_string_size(txt));

        furi_string_free(txt);
    }
}

static void
    discovery_bind_service(const DiscoveryInterface* interface, DiscoveryService* service) {
    furi_assert(interface);
    furi_assert(service);
    LWIP_ASSERT_CORE_LOCKED();

    const DiscoveryServiceInfo* info = service->info;
    mdns_resp_add_service(
        interface->netif,
        info->name,
        info->service,
        discovery_transport_to_lwip(info->transport_type),
        info->port,
        discovery_txt_adapter,
        service);

    FURI_LOG_D(
        TAG,
        "Bound '%s' to netif '%c%c'",
        service->info->name,
        interface->netif->name[0],
        interface->netif->name[1]);
}

static void discovery_device_name_state_callback(const void* item, void* context) {
    furi_assert(item);
    furi_assert(context);

    const DeviceNameState* state = item;
    Discovery* discovery = context;

    const DiscoveryApiMessage api_message = {
        .type = DiscoveryApiMessageTypeDeviceName,
        .device_name_state = *state,
    };

    discovery_send_api_message(discovery, &api_message);
}

static void discovery_netif_up(Discovery* discovery, NetworkNetif netif_id) {
    furi_check(discovery);

    LOCK_TCPIP_CORE();
    struct netif* netif = network_find_netif(netif_id);
    FURI_LOG_D(TAG, "Network up: netif '%c%c'", netif->name[0], netif->name[1]);
    UNLOCK_TCPIP_CORE();

    DiscoveryInterface* interface = &discovery->interfaces[netif_id];

    if(!interface->netif) {
        interface->netif = netif;

        FuriString* hostname_furi = furi_string_alloc();
        const char* hostname = discovery_device_name_to_hostname(
            furi_string_get_cstr(discovery->device_name), hostname_furi);

        LOCK_TCPIP_CORE();

        mdns_resp_add_netif(netif, hostname);
        FURI_LOG_D(
            TAG, "Added netif '%c%c' with name '%s'", netif->name[0], netif->name[1], hostname);

        /* clang-format off */
        for M_EACH(service, discovery->services, DiscoveryServices_t) {
            discovery_bind_service(interface, service);
        }
        /* clang-format on */

        UNLOCK_TCPIP_CORE();
        furi_string_free(hostname_furi);
    }

    LOCK_TCPIP_CORE();
    mdns_resp_announce(netif);
    UNLOCK_TCPIP_CORE();

    FURI_LOG_D(TAG, "Announced to netif '%c%c'", netif->name[0], netif->name[1]);
}

static void
    discovery_add_service_handler(Discovery* discovery, const DiscoveryApiMessage* api_message) {
    const DiscoveryService* service_to_add = &api_message->service_to_add;
    FURI_LOG_I(TAG, "Service added: '%s'", service_to_add->info->name);

    DiscoveryService* service = DiscoveryServices_push_new(discovery->services);
    *service = *service_to_add;

    LOCK_TCPIP_CORE();

    for(size_t i = 0; i < COUNT_OF(discovery->interfaces); i++) {
        const DiscoveryInterface* interface = &discovery->interfaces[i];
        if(interface->netif == NULL) {
            continue;
        }

        discovery_bind_service(interface, service);
    }

    UNLOCK_TCPIP_CORE();
}

static void
    discovery_device_name_handler(Discovery* discovery, const DiscoveryApiMessage* api_message) {
    const char* new_device_name = api_message->device_name_state.name;

    if(furi_string_equal(discovery->device_name, new_device_name)) {
        return;
    }

    furi_string_set(discovery->device_name, new_device_name);

    FuriString* hostname_buf = furi_string_alloc();
    const char* hostname = discovery_device_name_to_hostname(new_device_name, hostname_buf);

    LOCK_TCPIP_CORE();

    for(size_t i = 0; i < COUNT_OF(discovery->interfaces); i++) {
        const DiscoveryInterface* interface = &discovery->interfaces[i];
        struct netif* netif = interface->netif;
        if(netif == NULL) {
            continue;
        }

        mdns_resp_rename_netif(netif, hostname);
        mdns_resp_announce(netif);

        FURI_LOG_D(
            TAG,
            "Renamed netif '%c%c' to '%s' & re-announced",
            netif->name[0],
            netif->name[1],
            hostname);
    }

    UNLOCK_TCPIP_CORE();

    furi_string_free(hostname_buf);
}

static void
    discovery_usb_network_handler(Discovery* discovery, const DiscoveryApiMessage* api_message) {
    if(api_message->usb_network_state == UsbNetworkStateUp) {
        discovery_netif_up(discovery, NetworkNetifUsb);
    }
}

static void
    discovery_wifi_network_handler(Discovery* discovery, const DiscoveryApiMessage* api_message) {
    const WifiState wifi_state = api_message->wifi_network_state;
    /* Restrict mdns events to Wifi state changes only. */
    if(discovery->wifi_state != wifi_state) {
        if(wifi_state == WifiStateConnected) {
            discovery_netif_up(discovery, NetworkNetifWifi);
        }
        discovery->wifi_state = wifi_state;
    }
}

static void discovery_api_message_queue_callback(FuriEventLoopObject* object, void* context) {
    furi_assert(context);
    Discovery* discovery = context;

    furi_assert(object == discovery->api_queue);
    FuriMessageQueue* api_queue = object;

    DiscoveryApiMessage api_message;
    while(furi_message_queue_get(api_queue, &api_message, 0) == FuriStatusOk) {
        const DiscoveryApiMessageType type = api_message.type;
        // TODO: handler array
        if(type == DiscoveryApiMessageTypeAddService) {
            discovery_add_service_handler(discovery, &api_message);
        } else if(type == DiscoveryApiMessageTypeDeviceName) {
            discovery_device_name_handler(discovery, &api_message);
        } else if(type == DiscoveryApiMessageTypeUsbNetwork) {
            discovery_usb_network_handler(discovery, &api_message);
        } else if(type == DiscoveryApiMessageTypeWifiNetwork) {
            discovery_wifi_network_handler(discovery, &api_message);
        } else {
            furi_crash("Invalid DiscoveryApiMessageType value");
        }
    }
}

// =======================
// Network driver adapters
// =======================

static void discovery_wifi_state_callback(const void* item, void* context) {
    furi_assert(item);
    furi_assert(context);
    const WifiInfo* info = item;
    Discovery* discovery = context;

    const DiscoveryApiMessage api_message = {
        .type = DiscoveryApiMessageTypeWifiNetwork,
        .wifi_network_state = info->state,
    };

    discovery_send_api_message(discovery, &api_message);
}

static void discovery_usb_network_state_callback(const void* item, void* context) {
    furi_assert(item);
    furi_assert(context);
    const UsbNetworkInfo* info = item;
    Discovery* discovery = context;

    const DiscoveryApiMessage api_message = {
        .type = DiscoveryApiMessageTypeUsbNetwork,
        .usb_network_state = info->state,
    };

    discovery_send_api_message(discovery, &api_message);
}

static void discovery_init_mdns(Discovery* discovery) {
    UNUSED(discovery);

    Network* network = furi_record_open(RECORD_NETWORK);
    network_init_current_thread(network);

    LOCK_TCPIP_CORE();
    mdns_resp_init();
    UNLOCK_TCPIP_CORE();
}

static void discovery_subscribe_to_network_state(Discovery* discovery) {
    furi_assert(discovery);

    Wifi* wifi = furi_record_open(RECORD_WIFI);
    furi_state_subscribe(wifi_get_state(wifi), discovery_wifi_state_callback, discovery);

    UsbNetwork* usb_network = furi_record_open(RECORD_USB_NETWORK);
    furi_state_subscribe(
        usb_network_get_state(usb_network), discovery_usb_network_state_callback, discovery);
}

static void discovery_busybar_txt(FuriString* txt_out, void* context) {
    furi_assert(context);
    Discovery* discovery = context;

    furi_string_printf(txt_out, "name=%s", furi_string_get_cstr(discovery->device_name));
}

static void discovery_add_device_service(Discovery* discovery) {
    const uint8_t* usb_mac = furi_hal_version_get_usb_mac();

    for(size_t i = 0; i < FURI_HAL_VERSION_MAC_LENGTH; i++) {
        snprintf(discovery->device_service_name + (i * 2), 3, "%02hhx", usb_mac[i]);
    }

    discovery->device_discovery = (const DiscoveryServiceInfo){
        .name = discovery->device_service_name,
        .service = "_busybar",
        .txt_callback = discovery_busybar_txt,
        .transport_type = DiscoveryTransportTypeTcp,
        .port = 0,
    };

    discovery_add_service(discovery, &discovery->device_discovery, discovery);
}

// ===============
// Service startup
// ===============

static Discovery* discovery_alloc(void) {
    Discovery* discovery = malloc(sizeof(Discovery));

    discovery->event_loop = furi_event_loop_alloc();
    discovery->api_queue = furi_message_queue_alloc(8, sizeof(DiscoveryApiMessage));
    DiscoveryServices_init(discovery->services);
    discovery->device_name = furi_string_alloc();

    furi_event_loop_subscribe_message_queue(
        discovery->event_loop,
        discovery->api_queue,
        FuriEventLoopEventIn,
        discovery_api_message_queue_callback,
        discovery);

    DeviceName* device_name = furi_record_open(RECORD_DEVICE_NAME);
    furi_state_subscribe(
        device_name_get_state(device_name), discovery_device_name_state_callback, discovery);

    discovery_init_mdns(discovery);
    discovery_subscribe_to_network_state(discovery);
    discovery_add_device_service(discovery);

    furi_record_create(RECORD_DISCOVERY, discovery);

    return discovery;
}

int32_t discovery_srv(void* arg) {
    UNUSED(arg);

    Discovery* discovery = discovery_alloc();
    furi_event_loop_run(discovery->event_loop);

    return 0;
}

// =======================
// Public API for services
// =======================

void discovery_add_service(Discovery* discovery, const DiscoveryServiceInfo* info, void* context) {
    furi_check(discovery);
    furi_check(info);
    furi_check(info->name);
    furi_check(info->service);

    /* clang-format off */
    const DiscoveryApiMessage api_message = {
        .type = DiscoveryApiMessageTypeAddService,
        .service_to_add = {
            .info = info,
            .context = context,
        },
    };
    /* clang-format on */

    discovery_send_api_message(discovery, &api_message);
}
