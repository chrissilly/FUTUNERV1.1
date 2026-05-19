#include "file_commands.h"
#include "filesystem/fs_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "FILE_CMD";

/* ── Path validation (C3 fix: path traversal protection) ────── */

static bool validate_path(const char *path, char *response, size_t response_size)
{
    if (!path || path[0] == '\0') {
        snprintf(response, response_size, "Empty path");
        return false;
    }
    if (strlen(path) > 128) {
        snprintf(response, response_size, "Path too long (max 128)");
        return false;
    }
    if (strstr(path, "..") != NULL) {
        snprintf(response, response_size, "Path traversal not allowed");
        return false;
    }
    if (path[0] == '/' && path[1] == '/') {
        snprintf(response, response_size, "Invalid path");
        return false;
    }
    return true;
}

/* ── Helper: extract and copy path from JSON params ─────────── */
/* Returns true if path was extracted and copied into path_buf.
 * Deletes root and writes error response on failure.
 * Caller must still call cJSON_Delete(root) on success. */

static bool extract_path(cJSON *root, char *path_buf, size_t path_buf_size,
                         char *response, size_t response_size)
{
    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path_item)) {
        snprintf(response, response_size, "Missing 'path' parameter");
        return false;
    }
    /* Copy path before any cJSON_Delete to avoid use-after-free (C4 fix) */
    strncpy(path_buf, path_item->valuestring, path_buf_size - 1);
    path_buf[path_buf_size - 1] = '\0';

    if (!validate_path(path_buf, response, response_size)) {
        return false;
    }
    return true;
}

esp_err_t cmd_fs_info(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0, used = 0;
    esp_err_t err = fs_manager_get_info(FS_PARTITION_STORAGE, &total, &used);

    if (err == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "mount_point",
                               fs_manager_get_mount_point(FS_PARTITION_STORAGE));
        cJSON_AddNumberToObject(root, "total_bytes", total);
        cJSON_AddNumberToObject(root, "used_bytes", used);
        cJSON_AddNumberToObject(root, "free_bytes", total - used);
        cJSON_AddNumberToObject(root, "used_percent", (total > 0) ? ((used * 100) / total) : 0);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
    } else {
        snprintf(response, response_size, "Failed to get filesystem info");
    }

    return err;
}

esp_err_t cmd_fs_list(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    /* C4 fix: copy path into local buffer before freeing cJSON */
    char path[132] = "/";

    if (params) {
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *path_item = cJSON_GetObjectItem(root, "path");
            if (cJSON_IsString(path_item)) {
                strncpy(path, path_item->valuestring, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
            cJSON_Delete(root);
        }
    }

    /* C3 fix: validate path */
    if (!validate_path(path, response, response_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    fs_entry_info_t *entries = heap_caps_malloc(64 * sizeof(fs_entry_info_t), MALLOC_CAP_SPIRAM);
    if (!entries) entries = malloc(64 * sizeof(fs_entry_info_t));
    if (!entries) {
        snprintf(response, response_size, "{\"error\":\"out of memory\"}");
        return ESP_ERR_NO_MEM;
    }
    size_t entry_count = 0;

    esp_err_t err = fs_manager_list_directory(FS_PARTITION_STORAGE, path,
                                              entries, 64, &entry_count);

    if (err == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "path", path);
        cJSON_AddNumberToObject(root, "count", entry_count);

        cJSON *entries_array = cJSON_CreateArray();
        for (size_t i = 0; i < entry_count; i++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "name", entries[i].name);
            cJSON_AddStringToObject(entry, "type", entries[i].is_directory ? "directory" : "file");
            cJSON_AddNumberToObject(entry, "size", entries[i].size);
            cJSON_AddNumberToObject(entry, "modified", entries[i].modified_time);
            cJSON_AddItemToArray(entries_array, entry);
        }
        cJSON_AddItemToObject(root, "entries", entries_array);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
    } else {
        snprintf(response, response_size, "Failed to list directory");
    }

    free(entries);
    return err;
}

esp_err_t cmd_fs_read(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /* C4 fix: copy path into local buffer before any cJSON operations that might free */
    char path[132];
    if (!extract_path(root, path, sizeof(path), response, response_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    size_t file_size = 0;
    esp_err_t err = fs_manager_get_file_size(FS_PARTITION_STORAGE, path, &file_size);
    if (err != ESP_OK || file_size == 0) {
        snprintf(response, response_size, "File not found or empty");
        return ESP_FAIL;
    }

    if (file_size > 32768) {
        snprintf(response, response_size, "File too large (max 32KB)");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *file_buffer = malloc(file_size);
    if (!file_buffer) {
        snprintf(response, response_size, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = 0;
    err = fs_manager_read_file(FS_PARTITION_STORAGE, path, file_buffer, file_size, &bytes_read);

    if (err == ESP_OK) {
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, file_buffer, bytes_read);

        char *b64_data = malloc(b64_len + 1);
        if (b64_data) {
            mbedtls_base64_encode((unsigned char *)b64_data, b64_len, &b64_len,
                                 file_buffer, bytes_read);
            b64_data[b64_len] = '\0';

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "path", path);
            cJSON_AddNumberToObject(resp, "size", bytes_read);
            cJSON_AddStringToObject(resp, "data", b64_data);

            char *json_str = cJSON_PrintUnformatted(resp);
            if (json_str) {
                strncpy(response, json_str, response_size - 1);
                response[response_size - 1] = '\0';
                free(json_str);
            }
            cJSON_Delete(resp);
            free(b64_data);
        }
    } else {
        snprintf(response, response_size, "Failed to read file");
    }

    free(file_buffer);
    return err;
}

esp_err_t cmd_fs_write(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /* C4 fix: copy path before freeing cJSON */
    char path[132];
    if (!extract_path(root, path, sizeof(path), response, response_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data_item)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing 'data' parameter");
        return ESP_ERR_INVALID_ARG;
    }

    const char *b64_data = data_item->valuestring;
    size_t b64_str_len = strlen(b64_data);

    size_t data_len = 0;
    mbedtls_base64_decode(NULL, 0, &data_len, (const unsigned char *)b64_data, b64_str_len);

    uint8_t *file_data = malloc(data_len);
    if (!file_data) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    int ret = mbedtls_base64_decode(file_data, data_len, &data_len,
                                    (const unsigned char *)b64_data, b64_str_len);

    /* Safe to free cJSON now — we've decoded the base64 data */
    cJSON_Delete(root);

    esp_err_t err = ESP_FAIL;
    if (ret == 0) {
        err = fs_manager_write_file(FS_PARTITION_STORAGE, path, file_data, data_len);
        if (err == ESP_OK) {
            snprintf(response, response_size, "{\"path\":\"%s\",\"size\":%d}", path, (int)data_len);
        } else {
            snprintf(response, response_size, "Failed to write file");
        }
    } else {
        snprintf(response, response_size, "Invalid base64 data");
    }

    free(file_data);
    return err;
}

esp_err_t cmd_fs_delete(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /* C4 fix: copy path into local buffer */
    char path[132];
    if (!extract_path(root, path, sizeof(path), response, response_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    esp_err_t err = fs_manager_delete_file(FS_PARTITION_STORAGE, path);
    if (err != ESP_OK) {
        err = fs_manager_delete_directory(FS_PARTITION_STORAGE, path);
    }

    if (err == ESP_OK) {
        snprintf(response, response_size, "Deleted successfully");
    } else {
        snprintf(response, response_size, "Failed to delete: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t cmd_fs_mkdir(int fd, const char *params, char *response, size_t response_size) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        snprintf(response, response_size, "Filesystem not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /* C4 fix: copy path into local buffer */
    char path[132];
    if (!extract_path(root, path, sizeof(path), response, response_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    esp_err_t err = fs_manager_create_directory(FS_PARTITION_STORAGE, path);

    if (err == ESP_OK) {
        snprintf(response, response_size, "Directory created successfully");
    } else {
        snprintf(response, response_size, "Failed to create directory");
    }

    return err;
}
