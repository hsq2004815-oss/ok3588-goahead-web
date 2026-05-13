#include "media_cleanup.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <ftw.h>
#include <unistd.h>
#include <errno.h>
#include "cJSON.h"

/* 删除文件的回调函数，用于ftw */
static int remove_file_callback(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    int ret = remove(path);
    if (ret != 0) {
        /* 记录错误但继续尝试删除其他文件 */
        trace(1, "无法删除文件 %s: %s", path, strerror(errno));
    }
    return 0; /* 继续处理下一个文件 */
}

/* 递归清空目录中的文件 */
static int clear_directory(const char *dir_path, int *error_count) {
    /* 验证输入 */
    if (!dir_path || !*dir_path) {
        if (error_count) (*error_count)++;
        return -1;
    }

    /* 检查目录是否存在 */
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (error_count) (*error_count)++;
        trace(1, "路径不存在或不是目录: %s", dir_path);
        return -1;
    }

    /* 使用nftw递归遍历目录并删除文件 */
    int flags = FTW_DEPTH | FTW_PHYS;
    int result = nftw(dir_path, remove_file_callback, 64, flags);
    
    if (result != 0 && error_count) {
        (*error_count)++;
    }
    
    return result;
}

/**
 * 清理指定类型的所有媒体文件
 * @param wp   Web请求结构体
 * @param type 媒体类型，图片或视频
 */
void clearAllMedia(Webs *wp, MediaType type) {
    char *srcDir = NULL;                 /* 源文件目录 */
    char *processedDir = NULL;           /* 处理后文件目录 */
    char *mediaTypeStr = NULL;           /* 媒体类型字符串 */
    int srcDirErrors = 0;                /* 源目录清理错误计数 */
    int procDirErrors = 0;               /* 处理后目录清理错误计数 */
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    
    /* 设置HTTP响应头 */
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    /* 根据媒体类型设置相关参数 */
    if (type == MEDIA_TYPE_IMAGE) {
        srcDir = "web/images";
        processedDir = "web/processed";
        mediaTypeStr = "图片";
    } else if (type == MEDIA_TYPE_VIDEO) {
        srcDir = "web/videos";
        processedDir = "web/processed_videos";
        mediaTypeStr = "视频";
    } else {
        /* 无效的媒体类型 */
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
    
    trace(2, "开始清理所有%s文件", mediaTypeStr);
    
    /* 清空原始文件目录 */
    trace(2, "清理原始%s目录: %s", mediaTypeStr, srcDir);
    int srcResult = clear_directory(srcDir, &srcDirErrors);
    
    /* 清空处理后文件目录 */
    trace(2, "清理处理后%s目录: %s", mediaTypeStr, processedDir);
    int procResult = clear_directory(processedDir, &procDirErrors);
    
    /* 创建JSON响应 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();
    
    /* 检查清理结果 */
    if (srcResult == 0 && procResult == 0) {
        /* 完全成功 */
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", sfmt("成功清理所有%s", mediaTypeStr));
    } else {
        /* 部分或全部失败 */
        cJSON_AddNumberToObject(response, "resp_code", 1);
        if (srcResult != 0 && procResult != 0) {
            cJSON_AddStringToObject(response, "resp_msg", 
                                   sfmt("清理%s和处理后%s目录均失败", mediaTypeStr, mediaTypeStr));
        } else if (srcResult != 0) {
            cJSON_AddStringToObject(response, "resp_msg", 
                                   sfmt("清理%s目录失败", mediaTypeStr));
        } else {
            cJSON_AddStringToObject(response, "resp_msg", 
                                   sfmt("清理处理后%s目录失败", mediaTypeStr));
        }
        /* 添加详细错误信息 */
        cJSON_AddNumberToObject(datas, "srcDirErrors", srcDirErrors);
        cJSON_AddNumberToObject(datas, "procDirErrors", procDirErrors);
    }
    
    trace(2, "%s清理完成，%s", mediaTypeStr, 
          (srcResult == 0 && procResult == 0) ? "完全成功" : "有错误发生");
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
} 