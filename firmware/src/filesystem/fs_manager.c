#include "fs_manager.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "FS_MGR";

static const fs_partition_config_t partition_configs[FS_PARTITION_COUNT] = {
    [FS_PARTITION_STORAGE] = {
        .partition_label = "cal",
        .mount_point = "/cal",
        .max_files = 32,
        .format_if_failed = true
    }
};

static bool partition_mounted[FS_PARTITION_COUNT] = {false};

esp_err_t fs_manager_init(void) {
    ESP_LOGI(TAG, "Filesystem manager initialized");
    return ESP_OK;
}

esp_err_t fs_manager_deinit(void) {
    for (fs_partition_id_t i = 0; i < FS_PARTITION_COUNT; i++) {
        if (partition_mounted[i]) {
            fs_manager_unmount_partition(i);
        }
    }
    return ESP_OK;
}

esp_err_t fs_manager_mount_partition(fs_partition_id_t partition_id) {
    if (partition_id >= FS_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (partition_mounted[partition_id]) {
        ESP_LOGW(TAG, "Partition %s already mounted", partition_configs[partition_id].partition_label);
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = partition_configs[partition_id].mount_point,
        .partition_label = partition_configs[partition_id].partition_label,
        .format_if_mount_failed = partition_configs[partition_id].format_if_failed,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition %s", 
                     partition_configs[partition_id].partition_label);
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(partition_configs[partition_id].partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition '%s' mounted at '%s'", 
                 partition_configs[partition_id].partition_label,
                 partition_configs[partition_id].mount_point);
        ESP_LOGI(TAG, "  Total: %d bytes, Used: %d bytes, Free: %d bytes",
                 total, used, total - used);
    }

    partition_mounted[partition_id] = true;
    return ESP_OK;
}

esp_err_t fs_manager_unmount_partition(fs_partition_id_t partition_id) {
    if (partition_id >= FS_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!partition_mounted[partition_id]) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(partition_configs[partition_id].partition_label);
    if (ret == ESP_OK) {
        partition_mounted[partition_id] = false;
        ESP_LOGI(TAG, "Partition '%s' unmounted", partition_configs[partition_id].partition_label);
    }

    return ret;
}

bool fs_manager_is_mounted(fs_partition_id_t partition_id) {
    if (partition_id >= FS_PARTITION_COUNT) {
        return false;
    }
    return partition_mounted[partition_id];
}

const char* fs_manager_get_mount_point(fs_partition_id_t partition_id) {
    if (partition_id >= FS_PARTITION_COUNT) {
        return NULL;
    }
    return partition_configs[partition_id].mount_point;
}

esp_err_t fs_manager_get_info(fs_partition_id_t partition_id, 
                               size_t *total_bytes, 
                               size_t *used_bytes) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_littlefs_info(partition_configs[partition_id].partition_label, 
                            total_bytes, used_bytes);
}

esp_err_t fs_manager_list_directory(fs_partition_id_t partition_id,
                                    const char *path,
                                    fs_entry_info_t *entries,
                                    size_t max_entries,
                                    size_t *entry_count) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    if (path && strlen(path) > 0 && strcmp(path, "/") != 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", 
                 partition_configs[partition_id].mount_point, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", 
                 partition_configs[partition_id].mount_point);
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return ESP_FAIL;
    }

    *entry_count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL && *entry_count < max_entries) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        fs_entry_info_t *info = &entries[*entry_count];
        strncpy(info->name, entry->d_name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        
        char entry_path[FS_MAX_PATH_LEN * 2];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) == 0) {
            info->size = st.st_size;
            info->is_directory = S_ISDIR(st.st_mode);
            info->modified_time = st.st_mtime;
        } else {
            info->size = 0;
            info->is_directory = (entry->d_type == DT_DIR);
            info->modified_time = 0;
        }

        (*entry_count)++;
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t fs_manager_read_file(fs_partition_id_t partition_id,
                               const char *path,
                               uint8_t *buffer,
                               size_t buffer_size,
                               size_t *bytes_read) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        return ESP_FAIL;
    }

    *bytes_read = fread(buffer, 1, buffer_size, f);
    fclose(f);

    return ESP_OK;
}

esp_err_t fs_manager_write_file(fs_partition_id_t partition_id,
                                const char *path,
                                const uint8_t *data,
                                size_t data_len) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if (written != data_len) {
        ESP_LOGE(TAG, "Write size mismatch: %d/%d", written, data_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Written %d bytes to %s", written, path);
    return ESP_OK;
}

esp_err_t fs_manager_delete_file(fs_partition_id_t partition_id,
                                 const char *path) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    if (unlink(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted file: %s", path);
    return ESP_OK;
}

esp_err_t fs_manager_create_directory(fs_partition_id_t partition_id,
                                      const char *path) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    if (mkdir(full_path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create directory: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created directory: %s", path);
    return ESP_OK;
}

esp_err_t fs_manager_delete_directory(fs_partition_id_t partition_id,
                                      const char *path) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    if (rmdir(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete directory: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted directory: %s", path);
    return ESP_OK;
}

bool fs_manager_file_exists(fs_partition_id_t partition_id,
                            const char *path) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return false;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    struct stat st;
    return (stat(full_path, &st) == 0);
}

esp_err_t fs_manager_get_file_size(fs_partition_id_t partition_id,
                                   const char *path,
                                   size_t *size) {
    if (partition_id >= FS_PARTITION_COUNT || !partition_mounted[partition_id]) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[FS_MAX_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", 
             partition_configs[partition_id].mount_point, path);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        return ESP_FAIL;
    }

    *size = st.st_size;
    return ESP_OK;
}

