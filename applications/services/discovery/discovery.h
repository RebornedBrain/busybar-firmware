/**
 * @file discovery.h
 * @brief Facilitates discovery of this device on the local network using mDNS.
 */
#pragma once

#include <core/string.h>

#define RECORD_DISCOVERY "discovery"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Discovery Discovery;

typedef void (*DiscoveryTxtCallback)(FuriString* txt_out, void* context);

typedef enum {
    DiscoveryTransportTypeTcp,
    DiscoveryTransportTypeUdp,
} DiscoveryTransportType;

typedef struct {
    const char* name;
    const char* service;
    DiscoveryTxtCallback txt_callback;
    DiscoveryTransportType transport_type;
    uint16_t port;
} DiscoveryServiceInfo;

// ================
// API for services
// ================

/**
 * @brief Add a service to be announced to the local network.
 *
 * @warning The data pointed to by @p info and @c context must live forever
 *          (i.e. must NOT be deleted after the call to this function).
 *
 * @param[in,out] discovery Discovery service
 * @param[in] info Service info to be announced
 * @param[in,out] context Context for @p txt_callback. May be @c NULL
 */
void discovery_add_service(Discovery* discovery, const DiscoveryServiceInfo* info, void* context);

#ifdef __cplusplus
}
#endif
