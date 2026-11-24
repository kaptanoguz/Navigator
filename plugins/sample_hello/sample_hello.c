/* Sample Navigator C ABI plugin (C11).
 * Demonstrates: command registration, file overlay badge, custom column, and text preview.
 */
#include <stdio.h>
#include <string.h>

#include <navigator/plugin.h>

static int g_invocations = 0;

static int cmd_hello(const char* current_dir, const char* const* selected_paths,
                     uint32_t selected_count, void* user_data) {
    (void)selected_paths;
    (void)selected_count;
    (void)user_data;
    if (current_dir == NULL || current_dir[0] == '\0') {
        return 1;
    }
    ++g_invocations;
    return 0;
}

static int has_suffix(const char* path, const char* suffix) {
    if (path == NULL || suffix == NULL) {
        return 0;
    }
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) {
        return 0;
    }
    return strcmp(path + (plen - slen), suffix) == 0;
}

static int sample_overlay(const char* path, NavigatorPluginOverlay* out, void* user_data) {
    (void)user_data;
    if (out == NULL || !has_suffix(path, ".navsample")) {
        return 0;
    }
    (void)snprintf(out->badge_text, sizeof(out->badge_text), "%s", "S");
    out->color_rgba = 0x4EC9B0FFu;
    (void)snprintf(out->tooltip, sizeof(out->tooltip), "%s", "Navigator Sample Plugin Badge");
    return 1;
}

static int sample_column(const char* path, char* buf, uint32_t buf_size, void* user_data) {
    (void)user_data;
    if (path == NULL || buf == NULL || buf_size == 0) {
        return 0;
    }
    const char* slash = strrchr(path, '/');
    const char* name = (slash != NULL) ? (slash + 1) : path;
    (void)snprintf(buf, buf_size, "%u chars", (unsigned)strlen(name));
    return 1;
}

static int sample_preview(const char* path, char* utf8_buf, uint32_t buf_size, void* user_data) {
    (void)user_data;
    if (!has_suffix(path, ".navsample") || utf8_buf == NULL || buf_size == 0) {
        return 0;
    }
    int n = snprintf(utf8_buf, buf_size, "Sample plugin preview for: %s", path);
    return n > 0 ? n : 0;
}

static const NavigatorPluginCommand kCommands[] = {
    {
        sizeof(NavigatorPluginCommand),
        "sample.hello",
        "Sample: Hello Plugin",
        "Demonstrates invoking a C ABI plugin command",
        cmd_hello,
        NULL,
    },
};

static const NavigatorPluginColumn kColumns[] = {
    {
        sizeof(NavigatorPluginColumn),
        "sample.name_len",
        "Name Length",
        100u,
        sample_column,
        NULL,
    },
};

static const NavigatorPluginInfo kPluginInfo = {
    NAVIGATOR_PLUGIN_ABI_VERSION,
    sizeof(NavigatorPluginInfo),
    "org.navigator.sample.hello",
    "Sample Hello Plugin",
    "1.0.0",
    "Reference C11 plugin exporting a command, badge overlay, column, and preview",
    kCommands,
    1u,
    sample_overlay,
    NULL,
    kColumns,
    1u,
    sample_preview,
    NULL,
    NULL,
    NULL,
};

NAVIGATOR_PLUGIN_EXPORT const NavigatorPluginInfo* navigator_plugin_get_info(void) {
    return &kPluginInfo;
}
