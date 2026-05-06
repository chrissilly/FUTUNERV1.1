#ifndef FS_MANAGER_H
#define FS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"
#include <dirent.h>

#define FS_MAX_PATH_LEN 256
#define FS_MAX_PARTITIONS 4

typedef enum {
    FS_PARTITION_STORAGE,
    FS_PARTITION_COUNT
} fs_partition_id_t;

typedef struct {
    const char *partition_label;
    const char *mount_point;
    size_t max_files;
    bool format_if_failed;
} fs_partition_config_t;

typedef struct {
    char name[64];
    size_t size;
    bool is_directory;
    time_t modified_time;
} fs_entry_info_t;

esp_err_t fs_manager_init(void);
esp_err_t fs_manager_deinit(void);

esp_err_t fs_manager_mount_partition(fs_partition_id_t partition_id);
esp_err_t fs_manager_unmount_partition(fs_partition_id_t partition_id);

bool fs_manager_is_mounted(fs_partition_id_t partition_id);
const char* fs_manager_get_mount_point(fs_partition_id_t partition_id);

esp_err_t fs_manager_get_info(fs_partition_id_t partition_id, 
                               size_t *total_bytes, 
                               size_t *used_bytes);

esp_err_t fs_manager_list_directory(fs_partition_id_t partition_id,
                                    const char *path,
                                    fs_entry_info_t *entries,
                                    size_t max_entries,
                                    size_t *entry_count);

esp_err_t fs_manager_read_file(fs_partition_id_t partition_id,
                               const char *path,
                               uint8_t *buffer,
                               size_t buffer_size,
                               size_t *bytes_read);

esp_err_t fs_manager_write_file(fs_partition_id_t partition_id,
                                const char *path,
                                const uint8_t *data,
                                size_t data_len);

esp_err_t fs_manager_delete_file(fs_partition_id_t partition_id,
                                 const char *path);

esp_err_t fs_manager_create_directory(fs_partition_id_t partition_id,
                                      const char *path);

esp_err_t fs_manager_delete_directory(fs_partition_id_t partition_id,
                                      const char *path);

bool fs_manager_file_exists(fs_partition_id_t partition_id,
                            const char *path);

esp_err_t fs_manager_get_file_size(fs_partition_id_t partition_id,
                                   const char *path,
                                   size_t *size);

#endif // FS_MANAGER_H

