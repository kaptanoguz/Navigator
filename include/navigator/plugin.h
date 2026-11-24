/* navigator/plugin.h
 *
 * Purpose: stable C ABI (v1) for dlopen()ed Navigator plugins.
 *          Capabilities: commands, file overlays (badges), custom Details columns, text previews.
 *          Process isolation is optional in v1 (in-process dlopen with strict ABI version and
 *          struct_size checks; plugins are never invoked during filesystem write operations).
 * Owner:   contracts.
 * Thread-safety: navigator_plugin_get_info() and shutdown_fn() are called on the UI thread.
 *                command callbacks run on the UI thread. overlay_fn, column value_fn, and
 *                preview_fn may be called on the UI thread or a worker thread; plugins should
 *                treat them as read-only and reentrant.
 *                The returned NavigatorPluginInfo must have static storage duration and stay
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

/* Command callback. Returns 0 on success, non-zero on error.
 * `current_dir` is the active tab's directory (NUL-terminated UTF-8).
 * `selected_paths` is an array of `selected_count` NUL-terminated UTF-8 paths (may be NULL if 0).
 */
typedef int (*NavigatorPluginCommandFn)(const char* current_dir, const char* const* selected_paths,
                                        uint32_t selected_count, void* user_data);

typedef struct NavigatorPluginCommand {
    uint32_t struct_size; /* sizeof(NavigatorPluginCommand) */
    const char* id;       /* unique reverse-DNS or plugin-prefixed id, e.g. "sample.hello" */
    const char* title;    /* display label for command palette */
    const char* description;
    NavigatorPluginCommandFn callback;
    void* user_data;
} NavigatorPluginCommand;

/* File overlay badge returned by a plugin for a path.
 * Strings are copied by the host immediately and must be NUL-terminated UTF-8 (or empty). */
typedef struct NavigatorPluginOverlay {
    char badge_text[16]; /* short UTF-8 badge, e.g. "★" or "OK" */
    uint32_t color_rgba; /* 0xRRGGBBAA; 0 = use theme accent */
    char tooltip[128];   /* optional hover tooltip */
} NavigatorPluginOverlay;

/* Returns 1 if an overlay was written to `out`, 0 if none applies. */
typedef int (*NavigatorPluginOverlayFn)(const char* path, NavigatorPluginOverlay* out,
                                        void* user_data);

/* Writes a NUL-terminated UTF-8 cell value into `buf` (up to `buf_size` bytes).
 * Returns 1 if a value was written, 0 for empty. */
typedef int (*NavigatorPluginColumnFn)(const char* path, char* buf, uint32_t buf_size,
                                       void* user_data);

typedef struct NavigatorPluginColumn {
    uint32_t struct_size; /* sizeof(NavigatorPluginColumn) */
    const char* id;       /* unique column id */
    const char* title;    /* column header text */
    uint32_t width_dp;    /* preferred column width in dp (0 = default 120) */
    NavigatorPluginColumnFn value_fn;
    void* user_data;
} NavigatorPluginColumn;

/* Writes a NUL-terminated UTF-8 text preview into `utf8_buf` (up to `buf_size` bytes).
 * Returns >0 (bytes written, excluding NUL) if handled, 0 if not handled. */
typedef int (*NavigatorPluginPreviewFn)(const char* path, char* utf8_buf, uint32_t buf_size,
                                        void* user_data);

/* Optional cleanup callback invoked on the UI thread before dlclose(). */
typedef void (*NavigatorPluginShutdownFn)(void* user_data);

typedef struct NavigatorPluginInfo {
    uint32_t abi_version; /* must equal NAVIGATOR_PLUGIN_ABI_VERSION */
    uint32_t struct_size; /* sizeof(NavigatorPluginInfo) as compiled by the plugin */
    const char* id;       /* reverse-DNS id, e.g. "org.example.hello" */
    const char* name;     /* display name */
    const char* version;  /* plugin version string, e.g. "0.1.0" */
    /* v1 capability tables (optional: NULL / 0 when unused; present when struct_size covers them)
     */
    const char* description;
    const NavigatorPluginCommand* commands;
    uint32_t command_count;
    NavigatorPluginOverlayFn overlay_fn;
    void* overlay_user_data;
    const NavigatorPluginColumn* columns;
    uint32_t column_count;
    NavigatorPluginPreviewFn preview_fn;
    void* preview_user_data;
    NavigatorPluginShutdownFn shutdown_fn;
    void* shutdown_user_data;
} NavigatorPluginInfo;

/* Signature of the entry point. Returns NULL to refuse loading. */
typedef const NavigatorPluginInfo* (*NavigatorPluginGetInfoFn)(void);
/* NOLINTEND(modernize-use-using,modernize-redundant-void-arg) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NAVIGATOR_PLUGIN_H */
