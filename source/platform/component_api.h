#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define KIRKWARE_COMPONENT_EXPORT __attribute__((visibility("default")))
#else
#define KIRKWARE_COMPONENT_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KIRKWARE_COMPONENT_ABI_VERSION 1u
#define KIRKWARE_COMPONENT_DETAIL_CAPACITY 160u

typedef struct KirkwareComponentStatus {
    uint32_t abi_version;
    uint32_t struct_size;
    int32_t tracer_pid;
    uint32_t mapped_file_count;
    uint64_t heartbeat;
    char detail[KIRKWARE_COMPONENT_DETAIL_CAPACITY];
} KirkwareComponentStatus;

typedef int (*KirkwareComponentInitializeFn)(KirkwareComponentStatus* status);
typedef int (*KirkwareComponentPollFn)(KirkwareComponentStatus* status);
typedef void (*KirkwareComponentShutdownFn)(void);

KIRKWARE_COMPONENT_EXPORT int
kirkware_component_initialize(KirkwareComponentStatus* status);
KIRKWARE_COMPONENT_EXPORT int
kirkware_component_poll(KirkwareComponentStatus* status);
KIRKWARE_COMPONENT_EXPORT void kirkware_component_shutdown(void);

#ifdef __cplusplus
}
#endif
