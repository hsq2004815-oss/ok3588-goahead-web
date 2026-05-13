/********************************* Includes ***********************************/
#include "modelmanage.h"
#include "cJSON.h"
#include <stdbool.h>

/********************************* Defines ************************************/
#define IMAGE_MODEL_DIR "../model/image"
#define VIDEO_MODEL_DIR "../model/video"
#define MAX_PATH_LENGTH 256
#define IMAGE_BUSINESS_TYPE 1
#define VIDEO_BUSINESS_TYPE 2

/********************************* Locals ************************************/
static char g_upload_temp_dir[MAX_PATH_LENGTH] = {0};

/********************************* Forwards ***********************************/

static void url_decode(char *str);
static int ensure_directory_exists(const char *path);
static int is_rknn_file(const char *filename);
static int move_file_to_directory(const char *src_path, const char *dest_dir, const char *filename);

/*********************************** Code *************************************/

/*
 * 检查目录是否存在，不存在则创建
 */
static int ensure_directory_exists(const char *path) {
    struct stat st = {0};
    
    if (stat(path, &st) == -1) {
        // 目录不存在，尝试创建
        if (mkdir(path, 0755) == -1) {
            logmsg(2, "创建目录失败: %s, 错误: %s", path, strerror(errno));
            return -1;
        }
        logmsg(2, "成功创建目录: %s", path);
    }
    
    return 0;
}

/*
 * 检查文件是否是RKNN模型文件
 */
static int is_rknn_file(const char *filename) {
    // 获取文件扩展名
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return 0; // false
    }
    
    // 检查是否为.rknn文件
    return strcmp(ext, ".rknn") == 0;
}

/*
 * 移动文件到目标目录
 */
static int move_file_to_directory(const char *src_path, const char *dest_dir, const char *filename) {
    char dest_path[MAX_PATH_LENGTH];
    
    // 构建目标文件路径
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, filename);
    
    // 先检查目标文件是否已存在
    struct stat st = {0};
    if (stat(dest_path, &st) == 0) {
        // 如果文件已存在，先删除
        if (unlink(dest_path) != 0) {
            logmsg(2, "无法删除已有文件: %s, 错误: %s", dest_path, strerror(errno));
            return -1;
        }
    }
    
    // 将文件从源路径复制到目标路径
    FILE *src_file = fopen(src_path, "rb");
    if (!src_file) {
        logmsg(2, "无法打开源文件: %s, 错误: %s", src_path, strerror(errno));
        return -1;
    }
    
    FILE *dest_file = fopen(dest_path, "wb");
    if (!dest_file) {
        fclose(src_file);
        logmsg(2, "无法创建目标文件: %s, 错误: %s", dest_path, strerror(errno));
        return -1;
    }
    
    // 复制文件内容
    char buffer[4096];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dest_file) != bytes_read) {
            fclose(src_file);
            fclose(dest_file);
            logmsg(2, "写入目标文件失败: %s", dest_path);
            return -1;
        }
    }
    
    fclose(src_file);
    fclose(dest_file);
    
    // 删除源文件
    if (unlink(src_path) != 0) {
        logmsg(2, "删除源文件失败: %s, 错误: %s", src_path, strerror(errno));
        // 这里不返回错误，因为文件已经成功复制
    }
    
    logmsg(2, "成功将模型文件移动到: %s", dest_path);
    return 0;
}

/**
 * 处理文件上传请求
 */
void importFile_fun(Webs *wp) {
    char *filename = NULL;
    char *business_type_str = NULL;
    char *temp_path = NULL;
    char *upload_dir = NULL;
    int business_type = 0;
    int isValid = 1;  // true
    cJSON *response = NULL;
    
    logmsg(2, "开始处理模型文件上传...");
    
    // 获取上传的文件名
    filename = websGetVar(wp, "fileName", NULL);
    if (!filename || strlen(filename) == 0) {
        logmsg(2, "未提供文件名");
        isValid = 0;  // false
        goto done;
    }
    
    // 检查是否为RKNN模型文件
    if (!is_rknn_file(filename)) {
        logmsg(2, "上传的文件不是RKNN模型: %s", filename);
        isValid = 0;  // false
        goto done;
    }
    
    // 获取业务类型参数
    business_type_str = websGetVar(wp, "business", "1");
    business_type = atoi(business_type_str);
    
    // 根据业务类型选择目标目录
    if (business_type == IMAGE_BUSINESS_TYPE) {
        upload_dir = IMAGE_MODEL_DIR;
        logmsg(2, "模型类型: 图像处理");
    } else if (business_type == VIDEO_BUSINESS_TYPE) {
        upload_dir = VIDEO_MODEL_DIR;
        logmsg(2, "模型类型: 视频处理");
    } else {
        logmsg(2, "无效的业务类型: %d", business_type);
        isValid = 0;  // false
        goto done;
    }
    
    // 检查并确保目标目录存在
    if (ensure_directory_exists(upload_dir) != 0) {
        logmsg(2, "无法确保目标目录存在: %s", upload_dir);
        isValid = 0;  // false
        goto done;
    }
    
    // 获取上传文件的临时路径（由GoAhead自动处理）
    temp_path = websGetVar(wp, "clientFilename", NULL);
    if (!temp_path) {
        logmsg(2, "无法获取上传文件的临时路径");
        isValid = 0;  // false
        goto done;
    }
    
    // 移动文件到目标目录
    if (move_file_to_directory(temp_path, upload_dir, filename) != 0) {
        logmsg(2, "移动文件失败");
        isValid = 0;  // false
        goto done;
    }
    
done:
    // 准备响应
    response = cJSON_CreateObject();
    
    if (isValid) {
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "message", "模型文件上传成功");
        
        // 构建文件URL路径
        char file_url[MAX_PATH_LENGTH];
        snprintf(file_url, sizeof(file_url), "/model/%s/%s", 
                (business_type == IMAGE_BUSINESS_TYPE) ? "image" : "video", 
                filename);
        cJSON_AddStringToObject(response, "url", file_url);
    } else {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "message", "模型文件上传失败");
    }
    
    // 将JSON响应发送给客户端
    char *json_string = cJSON_Print(response);
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWrite(wp, "%s", json_string);    /* 将JSON字符串写入响应 */
    free(json_string);
    cJSON_Delete(response);
}

/**
 * 初始化模型管理系统
 */
int initModelManageSystem(void) {
    // 确保模型目录存在
    if (ensure_directory_exists(IMAGE_MODEL_DIR) != 0 ||
        ensure_directory_exists(VIDEO_MODEL_DIR) != 0) {
        return -1;
    }
    
    logmsg(2, "模型管理系统初始化完成");
    return 0;
}

/***************************************************************************************************
** 函数名称: uploadModelChunk_fun
** 功能描述: 处理模型文件分片上传功能
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void uploadModelChunk_fun(Webs *wp)
{
    WebsKey         *s;               /* 定义哈希表的键指针，用于遍历上传的文件 */
    WebsUpload      *up;              /* 定义上传文件的结构体指针 */
    char            *fileName = NULL; /* 原始文件名 */
    char            *chunkIndex = NULL; /* 当前分片索引 */
    char            *totalChunks = NULL; /* 总分片数 */
    char            *tempFileId = NULL;  /* 临时文件ID */
    char            *businessType = NULL; /* 业务类型 */
    char            tempDir[MAX_PATH_LENGTH]; /* 临时目录路径 */
    char            chunkPath[MAX_PATH_LENGTH]; /* 分片文件路径 */
    char            cmd[MAX_PATH_LENGTH]; /* 系统命令 */
    char            *uploadDir = NULL;    /* 上传目录 */
    int             business_type = IMAGE_BUSINESS_TYPE; /* 业务类型数值，默认为图像处理 */
    cJSON           *response = NULL; /* JSON响应对象 */
    cJSON           *datas = NULL;    /* JSON数据对象 */
    char            *jsonString = NULL; /* JSON字符串 */
    int             chunkIdx = 0;     /* 分片索引数值 */
    char            uniqueId[64];     /* 唯一标识符 */
    struct timeval  tv;               /* 时间结构体，用于生成唯一ID */
    char            *uri, *query, *key, *value; /* URI解析用变量 */
    char            decodedValue[512] = {0};    /* 解码后的值 */
    bool            foundChunk = false;         /* 是否找到分片 */
    
    /* 添加详细的请求信息日志 */
    logmsg(2, "================================");
    logmsg(2, "模型上传请求详细信息:");
    logmsg(2, "- 方法: %s", wp->method);
    logmsg(2, "- URL: %s", wp->url);
    logmsg(2, "- 内容类型: %s", wp->contentType ? wp->contentType : "未知");
    logmsg(2, "- 内容长度: %d", wp->rxLen);
    logmsg(2, "- 剩余字节: %d", wp->rxRemaining);
    logmsg(2, "- 标志: 0x%x", wp->flags);
    logmsg(2, "================================");
    
    websSetStatus(wp, 200);           /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);      /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);          /* 结束HTTP头部写入 */

    /* 创建JSON响应对象 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();

    /* 1. 从URL查询字符串获取参数 */
    uri = sclone(wp->url);
    logmsg(2, "请求URL: %s", uri);
    
    if ((query = strchr(uri, '?')) != NULL) {
        *query++ = '\0';
        
        logmsg(2, "发现查询参数: %s", query);
        
        while (query && *query) {
            key = stok(query, "&", &query);
            if ((value = strchr(key, '=')) != NULL) {
                *value++ = '\0';
                
                /* 解码参数值 */
                strncpy(decodedValue, value, sizeof(decodedValue) - 1);
                decodedValue[sizeof(decodedValue) - 1] = '\0';
                url_decode(decodedValue);
                
                if (strcmp(key, "fileName") == 0 && *decodedValue) {
                    fileName = sclone(decodedValue);
                    logmsg(2, "从URL查询参数获取文件名: %s", fileName);
                } else if (strcmp(key, "chunkIndex") == 0 && *decodedValue) {
                    chunkIndex = sclone(decodedValue);
                    logmsg(2, "从URL查询参数获取分片索引: %s", chunkIndex);
                } else if (strcmp(key, "totalChunks") == 0 && *decodedValue) {
                    totalChunks = sclone(decodedValue);
                    logmsg(2, "从URL查询参数获取总分片数: %s", totalChunks);
                } else if (strcmp(key, "tempFileId") == 0 && *decodedValue) {
                    tempFileId = sclone(decodedValue);
                    logmsg(2, "从URL查询参数获取临时ID: %s", tempFileId);
                } else if (strcmp(key, "business") == 0 && *decodedValue) {
                    businessType = sclone(decodedValue);
                    logmsg(2, "从URL查询参数获取业务类型: %s", businessType);
                }
            }
        }
    }
    wfree(uri);
    
    /* 2. 尝试从表单参数获取（如果URL中没有提供） */
    if (!fileName) {
        fileName = sclone(websGetVar(wp, "fileName", NULL));
        if (fileName) {
            logmsg(2, "从表单参数获取文件名: %s", fileName);
        }
    }
    
    if (!chunkIndex) {
        chunkIndex = sclone(websGetVar(wp, "chunkIndex", NULL));
        if (chunkIndex) {
            logmsg(2, "从表单参数获取分片索引: %s", chunkIndex);
        }
    }
    
    if (!totalChunks) {
        totalChunks = sclone(websGetVar(wp, "totalChunks", NULL));
        if (totalChunks) {
            logmsg(2, "从表单参数获取总分片数: %s", totalChunks);
        }
    }
    
    if (!tempFileId) {
        tempFileId = sclone(websGetVar(wp, "tempFileId", NULL));
        if (tempFileId) {
            logmsg(2, "从表单参数获取临时ID: %s", tempFileId);
        }
    }
    
    if (!businessType) {
        businessType = sclone(websGetVar(wp, "business", "1"));  /* 默认为图像处理 */
        if (businessType) {
            logmsg(2, "从表单参数获取业务类型: %s", businessType);
        }
    }

    /* 校验参数 */
    if (!chunkIndex) {
        logmsg(2, "参数不完整，缺少分片索引");
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "参数不完整，缺少分片索引");
        goto done;
    }

    /* 尝试转换分片索引为数字 */
    chunkIdx = atoi(chunkIndex);
    if (chunkIdx < 0) {
        logmsg(2, "分片索引无效: %s", chunkIndex);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "分片索引无效");
        goto done;
    }

    /* 确定业务类型和上传目录 */
    if (businessType) {
        business_type = atoi(businessType);
    }
    
    if (business_type == IMAGE_BUSINESS_TYPE) {
        uploadDir = IMAGE_MODEL_DIR;
        logmsg(2, "模型类型: 图像处理");
    } else if (business_type == VIDEO_BUSINESS_TYPE) {
        uploadDir = VIDEO_MODEL_DIR;
        logmsg(2, "模型类型: 视频处理");
    } else {
        logmsg(2, "无效的业务类型: %d", business_type);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "无效的业务类型");
        goto done;
    }

    /* 确保模型目录存在 */
    ensure_directory_exists(IMAGE_MODEL_DIR);
    ensure_directory_exists(VIDEO_MODEL_DIR);

    /* 如果没有tempFileId（首个分片），则生成一个 */
    if (!tempFileId) {
        /* 使用时间戳和随机数生成唯一ID */
        gettimeofday(&tv, NULL);
        sprintf(uniqueId, "%ld_%ld_%d", (long)tv.tv_sec, (long)tv.tv_usec, rand());
        tempFileId = sclone(uniqueId);
        logmsg(2, "为新上传生成临时ID: %s", tempFileId);
    } else {
        logmsg(2, "使用现有临时ID: %s", tempFileId);
    }

    /* 创建临时目录 */
    sprintf(tempDir, "%s/tmp_%s", uploadDir, tempFileId);
    sprintf(cmd, "mkdir -p %s", tempDir);
    if (system(cmd) != 0) {
        logmsg(2, "创建临时目录失败: %s", tempDir);
        logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "创建临时目录失败");
        goto done;
    }

    /* 保存分片到临时目录 */
    sprintf(chunkPath, "%s/chunk_%d", tempDir, chunkIdx);
    
    /* 处理multipart/form-data上传 */
    if (wp->files && hashFirst(wp->files)) {
        logmsg(2, "处理上传文件分片...");
        
        for (s = hashFirst(wp->files); s; s = hashNext(wp->files, s)) {
            up = s->content.value.symbol;
            
            if (!up || !up->filename) {
                logmsg(2, "无效的上传文件结构");
                continue;
            }
            
            logmsg(2, "处理上传文件:");
            logmsg(2, "- 临时文件: %s", up->filename ? up->filename : "未知");
            logmsg(2, "- 客户端文件名: %s", up->clientFilename ? up->clientFilename : "未知");
            logmsg(2, "- 文件大小: %d", up->size);
            logmsg(2, "- 内容类型: %s", up->contentType ? up->contentType : "未知");
            
            /* 使用复制替代重命名，解决跨设备问题 */
            sprintf(cmd, "cp \"%s\" \"%s\"", up->filename, chunkPath);
            logmsg(2, "执行命令: %s", cmd);
            
            if (system(cmd) != 0) {
                logmsg(2, "保存分片失败: %s", chunkPath);
                logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
                continue;  // 尝试下一个文件
            }
            
            /* 删除临时文件 */
            sprintf(cmd, "rm \"%s\"", up->filename);
            system(cmd);
            
            foundChunk = true;
            break; /* 只处理第一个文件 */
        }
    } else {
        logmsg(2, "警告: 未找到上传文件集合");
        logmsg(2, "请检查请求是否包含文件数据，以及内容类型是否正确");
    }
    
    /* 检查是否找到并保存了分片 */
    if (!foundChunk) {
        logmsg(2, "未找到有效的分片数据");
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "未找到有效的分片数据");
        goto done;
    }
    
    /* 验证分片文件是否存在 */
    if (access(chunkPath, F_OK) != 0) {
        logmsg(2, "分片文件未成功保存: %s", chunkPath);
        logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "分片文件保存失败");
        goto done;
    }

    /* 检查分片文件大小 */
    struct stat st;
    if (stat(chunkPath, &st) == 0) {
        logmsg(2, "分片 %d 文件大小: %ld 字节", chunkIdx, (long)st.st_size);
    }

    /* 分片保存成功 */
    logmsg(2, "分片上传成功: 索引=%d, 路径=%s", chunkIdx, chunkPath);
    cJSON_AddNumberToObject(response, "resp_code", 0);
    cJSON_AddStringToObject(response, "resp_msg", "分片上传成功");
    cJSON_AddStringToObject(datas, "tempFileId", tempFileId);
    cJSON_AddNumberToObject(datas, "chunkIndex", chunkIdx);

done:
    /* 释放分配的内存 */
    if (fileName && fileName != websGetVar(wp, "fileName", NULL)) {
        wfree(fileName);
    }
    if (chunkIndex && chunkIndex != websGetVar(wp, "chunkIndex", NULL)) {
        wfree(chunkIndex);
    }
    if (totalChunks && totalChunks != websGetVar(wp, "totalChunks", NULL)) {
        wfree(totalChunks);
    }
    if (tempFileId && tempFileId != websGetVar(wp, "tempFileId", NULL)) {
        wfree(tempFileId);
    }
    if (businessType && businessType != websGetVar(wp, "business", "1")) {
        wfree(businessType);
    }
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    logmsg(2, "上传处理完成，响应: %s", jsonString);
    logmsg(2, "================================");
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: mergeModelChunks_fun
** 功能描述: 合并模型文件分片功能
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void mergeModelChunks_fun(Webs *wp)
{
    char            *requestBody = NULL; /* 请求体内容 */
    cJSON           *requestJson = NULL; /* 请求JSON对象 */
    char            *fileName = NULL;    /* 原始文件名 */
    char            *tempFileId = NULL;  /* 临时文件ID */
    char            *businessType = NULL; /* 业务类型 */
    int             totalChunks = 0;     /* 总分片数 */
    char            tempDir[MAX_PATH_LENGTH]; /* 临时目录路径 */
    char            targetPath[MAX_PATH_LENGTH]; /* 目标文件路径 */
    char            cmd[MAX_PATH_LENGTH];    /* 系统命令 */
    cJSON           *response = NULL;    /* JSON响应对象 */
    cJSON           *datas = NULL;       /* JSON数据对象 */
    char            *jsonString = NULL;  /* JSON字符串 */
    int             i;                   /* 循环计数器 */
    char            *uri, *query, *key, *value; /* URI 解析临时变量 */
    char            decodedFileName[512] = {0};   /* 解码后的文件名 */
    char            decodedTempFileId[512] = {0}; /* 解码后的临时ID */
    char            decodedChunks[64] = {0};      /* 解码后的分片数 */
    char            decodedBusiness[64] = {0};    /* 解码后的业务类型 */
    char            *savedTempFileId = NULL;      /* 保存的临时ID指针 */
    char            *uploadDir = NULL;             /* 上传目录 */
    int             business_type = IMAGE_BUSINESS_TYPE;  /* 业务类型数值，默认为图像处理 */
    
    /* 添加详细的请求信息日志 */
    logmsg(2, "================================");
    logmsg(2, "模型合并请求详细信息:");
    logmsg(2, "- 方法: %s", wp->method);
    logmsg(2, "- URL: %s", wp->url);
    logmsg(2, "- 内容类型: %s", wp->contentType ? wp->contentType : "未知");
    logmsg(2, "- 内容长度: %d", wp->rxLen);
    logmsg(2, "- 剩余字节: %d", wp->rxRemaining);
    logmsg(2, "- 标志: 0x%x", wp->flags);
    logmsg(2, "================================");
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    /* 创建JSON响应对象 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();
    
    /* 1. 从URL查询字符串获取参数 */
    uri = sclone(wp->url);
    if ((query = strchr(uri, '?')) != NULL) {
        *query++ = '\0';
        
        logmsg(2, "合并请求：发现URL查询参数: %s", query);
        
        while (query && *query) {
            key = stok(query, "&", &query);
            if ((value = strchr(key, '=')) != NULL) {
                *value++ = '\0';
                
                /* 根据参数名选择不同的缓冲区 */
                if (strcmp(key, "fileName") == 0 && *value) {
                    strncpy(decodedFileName, value, sizeof(decodedFileName) - 1);
                    decodedFileName[sizeof(decodedFileName) - 1] = '\0';
                    url_decode(decodedFileName);
                    fileName = decodedFileName;
                    logmsg(2, "从URL查询参数获取文件名: %s", fileName);
                } else if (strcmp(key, "tempFileId") == 0 && *value) {
                    strncpy(decodedTempFileId, value, sizeof(decodedTempFileId) - 1);
                    decodedTempFileId[sizeof(decodedTempFileId) - 1] = '\0';
                    url_decode(decodedTempFileId);
                    /* 使用strdup创建持久副本 */
                    savedTempFileId = strdup(decodedTempFileId);
                    tempFileId = savedTempFileId;
                    logmsg(2, "从URL查询参数获取临时ID: %s", tempFileId);
                } else if (strcmp(key, "totalChunks") == 0 && *value) {
                    strncpy(decodedChunks, value, sizeof(decodedChunks) - 1);
                    decodedChunks[sizeof(decodedChunks) - 1] = '\0';
                    url_decode(decodedChunks);
                    totalChunks = atoi(decodedChunks);
                    logmsg(2, "从URL查询参数获取总分片数: %d", totalChunks);
                } else if (strcmp(key, "business") == 0 && *value) {
                    strncpy(decodedBusiness, value, sizeof(decodedBusiness) - 1);
                    decodedBusiness[sizeof(decodedBusiness) - 1] = '\0';
                    url_decode(decodedBusiness);
                    businessType = decodedBusiness;
                    logmsg(2, "从URL查询参数获取业务类型: %s", businessType);
                }
            }
        }
    }
    wfree(uri);
    
    /* 2. 尝试从表单参数获取 */
    if (!fileName) {
        fileName = websGetVar(wp, "fileName", NULL);
        if (fileName) {
            logmsg(2, "从表单参数获取文件名: %s", fileName);
        }
    }
    
    if (!tempFileId) {
        tempFileId = websGetVar(wp, "tempFileId", NULL);
        if (tempFileId) {
            logmsg(2, "从表单参数获取临时ID: %s", tempFileId);
        }
    }
    
    if (totalChunks <= 0) {
        const char* chunksStr = websGetVar(wp, "totalChunks", NULL);
        if (chunksStr) {
            totalChunks = atoi(chunksStr);
            logmsg(2, "从表单参数获取总分片数: %d", totalChunks);
        }
    }
    
    if (!businessType) {
        businessType = websGetVar(wp, "business", "1");  /* 默认为图像处理 */
        if (businessType) {
            logmsg(2, "从表单参数获取业务类型: %s", businessType);
        }
    }
    
    /* 3. 最后从请求体获取参数 */
    if ((!fileName || !tempFileId || totalChunks <= 0 || !businessType) && wp->rxRemaining > 0) {
        requestBody = malloc(wp->rxRemaining + 1);
        if (requestBody) {
            memcpy(requestBody, wp->input.servp, wp->rxRemaining);
            requestBody[wp->rxRemaining] = '\0';
            
            logmsg(2, "合并请求体内容: %s", requestBody);
            
            /* 解析JSON */
            requestJson = cJSON_Parse(requestBody);
            if (requestJson) {
                cJSON *fileNameObj = cJSON_GetObjectItem(requestJson, "fileName");
                cJSON *tempFileIdObj = cJSON_GetObjectItem(requestJson, "tempFileId");
                cJSON *totalChunksObj = cJSON_GetObjectItem(requestJson, "totalChunks");
                cJSON *businessTypeObj = cJSON_GetObjectItem(requestJson, "business");
                
                if (!fileName && fileNameObj && cJSON_IsString(fileNameObj) && fileNameObj->valuestring) {
                    /* 创建文件名的本地副本 */
                    strncpy(decodedFileName, fileNameObj->valuestring, sizeof(decodedFileName) - 1);
                    decodedFileName[sizeof(decodedFileName) - 1] = '\0';
                    fileName = decodedFileName;
                    logmsg(2, "从JSON请求体获取文件名: %s", fileName);
                }
                
                if (!tempFileId && tempFileIdObj && cJSON_IsString(tempFileIdObj) && tempFileIdObj->valuestring) {
                    savedTempFileId = strdup(tempFileIdObj->valuestring);
                    tempFileId = savedTempFileId;
                    logmsg(2, "从JSON请求体获取临时ID: %s", tempFileId);
                }
                
                if (totalChunks <= 0 && totalChunksObj && cJSON_IsNumber(totalChunksObj)) {
                    totalChunks = totalChunksObj->valueint;
                    logmsg(2, "从JSON请求体获取总分片数: %d", totalChunks);
                }
                
                if (!businessType && businessTypeObj && cJSON_IsString(businessTypeObj) && businessTypeObj->valuestring) {
                    businessType = businessTypeObj->valuestring;
                    logmsg(2, "从JSON请求体获取业务类型: %s", businessType);
                }
            } else {
                logmsg(2, "JSON解析失败: %s", cJSON_GetErrorPtr());
            }
        } else {
            logmsg(2, "无法分配内存用于请求体");
        }
    }
    
    /* 输出最终获取到的参数 */
    logmsg(2, "最终合并参数 - 文件名: %s, 临时ID: %s, 总分片数: %d, 业务类型: %s", 
           fileName ? fileName : "未知", 
           tempFileId ? tempFileId : "未知", 
           totalChunks,
           businessType ? businessType : "未知");
    
    /* 校验参数 */
    if (!fileName || !tempFileId || totalChunks <= 0 || !businessType) {
        logmsg(2, "参数不完整或无效");
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "参数不完整或无效");
        goto done;
    }
    
    /* 确保模型目录存在 */
    ensure_directory_exists(IMAGE_MODEL_DIR);
    ensure_directory_exists(VIDEO_MODEL_DIR);
    
    /* 确定业务类型和上传目录 */
    if (businessType) {
        business_type = atoi(businessType);
    }
    
    if (business_type == IMAGE_BUSINESS_TYPE) {
        uploadDir = IMAGE_MODEL_DIR;
        logmsg(2, "模型类型: 图像处理");
    } else if (business_type == VIDEO_BUSINESS_TYPE) {
        uploadDir = VIDEO_MODEL_DIR;
        logmsg(2, "模型类型: 视频处理");
    } else {
        logmsg(2, "无效的业务类型: %d", business_type);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "无效的业务类型");
        goto done;
    }
    
    /* 构建临时目录路径和目标文件路径 */
    sprintf(tempDir, "%s/tmp_%s", uploadDir, tempFileId);
    sprintf(targetPath, "%s/%s", uploadDir, fileName);
    
    logmsg(2, "临时目录路径: %s", tempDir);
    logmsg(2, "目标文件路径: %s", targetPath);
    
    /* 检查临时目录是否存在 */
    if (access(tempDir, F_OK) != 0) {
        logmsg(2, "临时目录不存在: %s", tempDir);
        logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "临时目录不存在，上传可能已失效");
        goto done;
    }
    
    /* 检查是否所有分片都存在 */
    for (i = 0; i < totalChunks; i++) {
        char chunkPath[MAX_PATH_LENGTH];
        sprintf(chunkPath, "%s/chunk_%d", tempDir, i);
        
        if (access(chunkPath, F_OK) != 0) {
            logmsg(2, "缺少分片: %s", chunkPath);
            logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "缺少部分分片，请重新上传");
            goto done;
        } else {
            /* 检查分片文件大小 */
            struct stat st;
            if (stat(chunkPath, &st) == 0) {
                logmsg(2, "分片 %d 文件大小: %ld 字节", i, (long)st.st_size);
            }
        }
    }
    
    /* 合并所有分片到最终文件 */
    logmsg(2, "开始合并分片到: %s", targetPath);
    
    /* 如果目标文件已存在，先删除 */
    if (access(targetPath, F_OK) == 0) {
        sprintf(cmd, "rm -f \"%s\"", targetPath);
        logmsg(2, "删除已存在的目标文件: %s", cmd);
        system(cmd);
    }
    
    /* 首先创建空文件 */
    sprintf(cmd, "touch \"%s\"", targetPath);
    logmsg(2, "创建目标文件: %s", cmd);
    if (system(cmd) != 0) {
        logmsg(2, "创建目标文件失败");
        logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "创建目标文件失败");
        goto done;
    }
    
    /* 逐个追加分片内容 */
    for (i = 0; i < totalChunks; i++) {
        char chunkPath[MAX_PATH_LENGTH];
        sprintf(chunkPath, "%s/chunk_%d", tempDir, i);
        
        /* 使用cat命令将分片追加到目标文件 */
        memset(cmd, 0, sizeof(cmd));
        sprintf(cmd, "cat \"%s\" >> \"%s\"", chunkPath, targetPath);
        
        logmsg(2, "执行追加命令 %d/%d: %s", i+1, totalChunks, cmd);
        
        if (system(cmd) != 0) {
            logmsg(2, "合并分片 %d 失败", i);
            logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "合并分片失败");
            goto done;
        }
    }
    
    /* 验证合并后的文件是否存在且大小合理 */
    struct stat st;
    if (stat(targetPath, &st) != 0 || st.st_size == 0) {
        logmsg(2, "合并后的文件无效或为空: %s", targetPath);
        logmsg(2, "错误信息: %s (errno: %d)", strerror(errno), errno);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "合并后的文件无效");
        goto done;
    }
    
    logmsg(2, "合并成功！目标文件大小: %ld 字节", (long)st.st_size);
    
    /* 检查文件是否为有效的RKNN模型文件 */
    if (!is_rknn_file(targetPath)) {
        logmsg(2, "不是有效的RKNN模型文件: %s", targetPath);
        /* 删除无效文件 */
        remove(targetPath);
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "不是有效的RKNN模型文件");
        goto done;
    }
    
    /* 清理临时文件 */
    sprintf(cmd, "rm -rf \"%s\"", tempDir);
    logmsg(2, "清理临时目录: %s", cmd);
    system(cmd);
    
    /* 合并成功 */
    cJSON_AddNumberToObject(response, "resp_code", 0);
    cJSON_AddStringToObject(response, "resp_msg", "模型分片合并成功");
    cJSON_AddStringToObject(datas, "fileName", fileName);
    cJSON_AddStringToObject(datas, "filePath", targetPath);
    
    /* 构建文件URL路径 */
    char file_url[MAX_PATH_LENGTH];
    snprintf(file_url, sizeof(file_url), "/model/%s/%s", 
            (business_type == IMAGE_BUSINESS_TYPE) ? "image" : "video", 
            fileName);
    cJSON_AddStringToObject(datas, "url", file_url);
    
done:
    if (requestJson) {
        cJSON_Delete(requestJson);
    }
    if (requestBody) {
        free(requestBody);
    }
    
    /* 释放临时分配的字符串 */
    if (savedTempFileId) {
        free(savedTempFileId);
    }
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    logmsg(2, "合并处理完成，响应: %s", jsonString);
    logmsg(2, "================================");
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: url_decode
** 功能描述: 将URL编码的字符串解码为普通字符串
** 输　入  : str - URL编码的字符串(同时也是输出缓冲区)
** 输　出  : 无
** 全局变量: 无
** 调用模块: 无
***************************************************************************************************/
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    char a, b;
    
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && 
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
} 