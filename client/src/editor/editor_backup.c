// Map editor backups: Krunker v7.1.4 initAutoBackup / doBackup / importMapBackup (E:2445-2460,
// 6803) and its LocalDB (IndexedDB "Editor" / store "MapBackups", keyPath "timestamp"), listed
// and cleared by window 15 "Load Backup" (editor_panels.c).
//
// Storage: html_storage on both platforms (a file per key natively, localStorage on the web):
// "knEditorBackups" is the index ([{timestamp, name}], oldest first) and each backup is
// "knEditorBackup.<timestamp>" = Editor.save(true) + timestamp, the record v7 inserts.
// Intentional difference from v7.1.4: at most ED_BACKUP_MAX backups are kept (the oldest go):
// localStorage has a few MB where IndexedDB had no practical limit, and v7 never prunes.
#include "editor_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ED_BACKUP_INDEX "knEditorBackups"
#define ED_BACKUP_MAX 20

static double g_next; // when the next timed backup is due (page clock, s); 0 = timer off

static char *storage_text(const char *key) {
    const int n = html_storage_get(key, NULL, 0);
    if (n <= 0) return NULL;
    char *buf = malloc((size_t) n + 1);
    if (buf) html_storage_get(key, buf, (size_t) n + 1);
    return buf;
}

static cJSON *index_read(void) {
    char *text = storage_text(ED_BACKUP_INDEX);
    cJSON *list = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(list);
        list = cJSON_CreateArray();
    }
    return list;
}

static void index_write(const cJSON *list) {
    char *text = cJSON_PrintUnformatted(list);
    if (text) html_storage_set(ED_BACKUP_INDEX, text);
    free(text);
}

static void backup_key(const double timestamp, char *out, const size_t n) { snprintf(out, n, "knEditorBackup.%.0f", timestamp); }

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double) ts.tv_sec * 1000.0 + (double) (ts.tv_nsec / 1000000);
}

void ed_backup_do(void) {
    if (!g_ed.proj || !g_ed.proj->map_config) return;
    double ts = now_ms();
    cJSON *list = index_read();
    const cJSON *last = cJSON_GetArrayItem(list, cJSON_GetArraySize(list) - 1);
    if (last && ts <= ed_json_num(last, "timestamp", 0)) ts = ed_json_num(last, "timestamp", 0) + 1; // keyPath stays unique
    cJSON *doc = ed_save_doc();
    cJSON_AddNumberToObject(doc, "timestamp", ts);
    char *text = cJSON_PrintUnformatted(doc);
    char key[64];
    backup_key(ts, key, sizeof(key));
    if (text) html_storage_set(key, text);
    free(text);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "timestamp", ts);
    cJSON_AddStringToObject(entry, "name", ed_json_str(cJSON_GetObjectItemCaseSensitive(doc, "map"), "name", ""));
    cJSON_AddItemToArray(list, entry);
    cJSON_Delete(doc);
    while (cJSON_GetArraySize(list) > ED_BACKUP_MAX) {
        backup_key(ed_json_num(cJSON_GetArrayItem(list, 0), "timestamp", 0), key, sizeof(key));
        html_storage_set(key, NULL);
        cJSON_DeleteItemFromArray(list, 0);
    }
    index_write(list);
    cJSON_Delete(list);
    fprintf(stderr, "[editor] Backup Successful\n");
}

void ed_backup_reset_timer(const double now) {
    // initAutoBackup: setInterval(doBackup, backupTimer * 60000) while autoBackups is on
    g_next = editor_setting_bool("autoBackups") ? now + editor_setting_num("backupTimer", 10) * 60.0 : 0;
}

void ed_backup_tick(const double now) {
    if (!editor_setting_bool("autoBackups")) {
        g_next = 0;
        return;
    }
    if (g_next <= 0) ed_backup_reset_timer(now);
    if (now < g_next) return;
    ed_backup_do();
    ed_backup_reset_timer(now);
}

cJSON *ed_backup_list(void) { return index_read(); }

int ed_backup_load(const double timestamp) {
    char key[64];
    backup_key(timestamp, key, sizeof(key));
    char *text = storage_text(key);
    if (!text) return 0;
    const int ok = ed_map_import(text); // importMap(JSON.stringify(record)): the save wrapper
    free(text);
    return ok;
}

int ed_backup_clear(void) {
    cJSON *list = index_read();
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        char key[64];
        backup_key(ed_json_num(e, "timestamp", 0), key, sizeof(key));
        html_storage_set(key, NULL);
    }
    cJSON_Delete(list);
    html_storage_set(ED_BACKUP_INDEX, NULL);
    return 1;
}

void ed_backup_date(const double timestamp, char *out, const size_t n) {
    // toLocaleString() in en-US: "10/12/2026, 3:04:05 PM"
    const time_t t = (time_t) (timestamp / 1000.0);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(out, n, "%d/%d/%d, %d:%02d:%02d %s", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, h12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
}
