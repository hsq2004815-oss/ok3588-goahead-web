#include "mediacommon.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "cJSON.h"

void getMediaList(Webs *wp, MediaType type) {
    char *baseDir = NULL;
    char *mediaTypeStr = NULL;
    cJSON *response = NULL;
    cJSON *datas = NULL;
    cJSON *filesArray = NULL;
    char *jsonString = NULL;
    
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    /* 根据媒体类型设置相关参数 */
    if (type == MEDIA_TYPE_IMAGE) {
        baseDir = "web/images";  /* 图片存储目录 */
        mediaTypeStr = "image";
    } else if (type == MEDIA_TYPE_VIDEO) {
        baseDir = "web/videos";  /* 视频存储目录 */
        mediaTypeStr = "video";
    } else {
        /* 无效的媒体类型，返回错误响应 */
        response = cJSON_CreateObject();
        datas = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "无效的媒体类型");
        cJSON_AddStringToObject(response, "resp_target", "null");
        cJSON_AddItemToObject(response, "datas", datas);
        
        jsonString = cJSON_Print(response);
        websWrite(wp, "%s", jsonString);
        
        cJSON_Delete(response);
        free(jsonString);
        websDone(wp);
        return;
    }
    
    /* 创建响应JSON结构 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();
    filesArray = cJSON_CreateArray();
    
    /* 打开目录 */
    DIR *dir = opendir(baseDir);
    if (dir == NULL) {
        /* 目录打开失败 */
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "Failed to open directory");
        cJSON_AddStringToObject(response, "resp_target", "null");
        cJSON_AddItemToObject(response, "datas", datas);
        
        jsonString = cJSON_Print(response);
        websWrite(wp, "%s", jsonString);
        
        cJSON_Delete(response);
        free(jsonString);
        websDone(wp);
        return;
    }
    
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过.和..目录 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* 检查文件类型和扩展名 */
        int isValidFile = 0;
        char *ext = strrchr(entry->d_name, '.');
        
        if (ext != NULL) {
            if (type == MEDIA_TYPE_IMAGE && 
                (scaselesscmp(ext, ".jpg") == 0 || 
                 scaselesscmp(ext, ".jpeg") == 0 || 
                 scaselesscmp(ext, ".png") == 0 ||
                 scaselesscmp(ext, ".bmp") == 0 ||
                 scaselesscmp(ext, ".gif") == 0 ||
                 scaselesscmp(ext, ".webp") == 0 ||
                 scaselesscmp(ext, ".tiff") == 0 ||
                 scaselesscmp(ext, ".tif") == 0)) {
                isValidFile = 1;
            } else if (type == MEDIA_TYPE_VIDEO && 
                      (scaselesscmp(ext, ".mp4") == 0 || 
                       scaselesscmp(ext, ".avi") == 0 ||
                       scaselesscmp(ext, ".mkv") == 0 ||
                       scaselesscmp(ext, ".mov") == 0 ||
                       scaselesscmp(ext, ".wmv") == 0 ||
                       scaselesscmp(ext, ".flv") == 0)) {
                isValidFile = 1;
            }
        }
        
        if (isValidFile) {
            /* 获取文件状态信息 */
            char fullPath[512];
            struct stat fileStat;
            
            snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir, entry->d_name);
            if (stat(fullPath, &fileStat) == 0) {
                /* 创建文件JSON对象 */
                cJSON *fileObj = cJSON_CreateObject();
                cJSON_AddStringToObject(fileObj, "name", entry->d_name);
                cJSON_AddNumberToObject(fileObj, "size", fileStat.st_size);
                cJSON_AddNumberToObject(fileObj, "timestamp", fileStat.st_mtime);
                
                /* 检查该图片是否已经处理过 - 可以根据实际逻辑调整 */
                char processedPath[512];
                if (type == MEDIA_TYPE_IMAGE) {
                    sprintf(processedPath, "web/processed/%s", entry->d_name);
                } else {
                    sprintf(processedPath, "web/processed_videos/%s", entry->d_name);
                }
                
                if (access(processedPath, F_OK) != -1) {
                    struct stat proc_st;
                    if (stat(processedPath, &proc_st) == 0 && proc_st.st_size > 0) {
                        cJSON_AddBoolToObject(fileObj, "processed", 1); /* true */
                    } else {
                        cJSON_AddBoolToObject(fileObj, "processed", 0); /* false */
                    }
                } else {
                    cJSON_AddBoolToObject(fileObj, "processed", 0); /* false */
                }
                
                /* 添加到文件数组 */
                cJSON_AddItemToArray(filesArray, fileObj);
            }
        }
    }
    
    closedir(dir);
    
    /* 完成JSON响应 */
    cJSON_AddNumberToObject(response, "resp_code", 0);
    cJSON_AddStringToObject(response, "resp_msg", "成功");
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(datas, "files", filesArray);
    cJSON_AddStringToObject(datas, "mediaType", mediaTypeStr);
    cJSON_AddItemToObject(response, "datas", datas);
    
    /* 发送响应 */
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
} 