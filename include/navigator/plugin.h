/* navigator/plugin.h
 *
 * Purpose: stable C ABI for out-of-process-safe, dlopen()ed Navigator plugins. STUB: only the
 *          version handshake exists; capabilities are added in a later phase.
 * Owner:   contracts.
 * Thread-safety: the entry point is called once, on the UI thread, right after dlopen(). The
 *                returned NavigatorPluginInfo must have static storage duration and stay
 *                unchanged while the plugin is loaded.
 *
 * Rules: C11 and C++ compatible; fixed-width integer types only; strings are NUL-terminated UTF-8.
 * Structs only grow at the end; `struct_size` lets the host detect older layouts. A host refuses
 * a plugin whose abi_version differs from NAVIGATOR_PLUGIN_ABI_VERSION.
 */
#ifndef NAVIGATOR_PLUGIN_H
#define NAVIGATOR_PLUGIN_H

#include <stdint.h>

#define NAVIGATOR_PLUGIN_ABI_VERSION 1u

/* Name of the symbol the host looks up with dlsym(). */
#define NAVIGATOR_PLUGIN_ENTRY_SYMBOL "navigator_plugin_get_info"

#if defined(__GNUC__) || defined(__clang__)
#define NAVIGATOR_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define NAVIGATOR_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* C ABI: typedef and (void) are required for C11; C++ modernize checks do not apply. */
/* NOLINTBEGIN(modernize-use-using,modernize-redundant-void-arg) */
typedef struct NavigatorPluginInfo {
    uint32_t abi_version; /* must equal NAVIGATOR_PLUGIN_ABI_VERSION */
    uint32_t struct_size; /* sizeof(NavigatorPluginInfo) as compiled by the plugin */
    const char* id;       /* reverse-DNS id, e.g. "org.example.hello" */
    const char* name;     /* display name */
    const char* version;  /* plugin version string, e.g. "0.1.0" */
} NavigatorPluginInfo;

/* Signature of the entry point. Returns NULL to refuse loading. */
typedef const NavigatorPluginInfo* (*NavigatorPluginGetInfoFn)(void);
/* NOLINTEND(modernize-use-using,modernize-redundant-void-arg) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NAVIGATOR_PLUGIN_H */
