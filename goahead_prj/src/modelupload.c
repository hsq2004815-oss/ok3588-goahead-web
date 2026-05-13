/********************************* Includes ***********************************/

#include "goahead.h"        /* GoAhead核心头文件 */
#include "modelupload.h"    /* 当前模块的头文件 */
#include "cJSON.h"          /* JSON解析库 */
#include <stdio.h>          /* 标准输入输出函数 */
#include <stdlib.h>         /* 标准库函数 */
#include <string.h>         /* 字符串操作函数 */
#include <ctype.h>          /* 字符处理函数 */
#include <sys/types.h>      /* 系统类型定义 */
#include <sys/stat.h>       /* 文件状态相关 */
#include <dirent.h>         /* 目录操作函数 */
#include <unistd.h>         /* Unix标准函数 */
#include <pthread.h>        /* POSIX线程库 */
#include <errno.h>          /* 错误码定义 */
#include <time.h>           /* 时间操作相关函数 */
#include <fcntl.h>          /* 文件控制操作 */
#include <stdbool.h>        /* 布尔类型 */

/********************************* Defines ************************************/

#define MAX_RESPONSE_SIZE 1024             /* 最大响应大小 */
#define MAX_PATH_LENGTH 512                /* 最大路径长度 */
#define VALID_MODEL_EXT ".rknn"           /* 有效的模型文件扩展名 */

/********************************* Globals ************************************/

static pthread_mutex_t g_model_mutex = PTHREAD_MUTEX_INITIALIZER; /* 互斥锁，保护模型操作 */
static bool g_dirs_initialized = false;    /* 目录初始化标志 */

/********************************* Forwards ***********************************/

static int mkdir_p(const char *path, mode_t mode);     /* 递归创建目录函数声明 */
static void url_decode(char *str);                      /* URL解码函数声明 */
static int copyFileWithBuffer(const char *srcPath, const char *destPath); /* 文件复制函数声明 */
static int ensureDirectoryPermissions(const char *dirPath, mode_t mode); /* 确保目录权限函数声明 */
static char* build_fs_path(char* buffer, size_t buffer_size, const char* dir, const char* filename); /* 构建文件系统路径函数声明 */

/***************************************************************************************************
** 函数名称: mkdir_p
** 功能描述: 递归创建目录，类似于mkdir -p命令
** 输　入  : path - 要创建的目录路径, mode - 创建目录的权限模式
** 输　出  : 成功返回0，失败返回错误码
***************************************************************************************************/
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[MAX_PATH_LENGTH];
    char *p = NULL;
    size_t len;
    int ret = 0;

    if (!path || !*path) {
        return -1; /* 路径为空 */
    }

    /* 复制路径字符串，避免修改原始字符串 */
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    /* 删除末尾的'/'，除非这是根目录 '/' */
    if (tmp[len - 1] == '/' && len > 1) {
        tmp[len - 1] = 0;
    }

    /* 遍历路径字符串，递归创建目录 */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (access(tmp, F_OK) != 0) {
                ret = mkdir(tmp, mode);
                if (ret != 0 && errno != EEXIST) {
                    return ret;
                }
            }
            *p = '/';
        }
    }

    /* 创建最终目录 */
    if (access(tmp, F_OK) != 0) {
        ret = mkdir(tmp, mode);
        if (ret != 0 && errno != EEXIST) {
            return ret;
        }
    }

    return 0;
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

/***************************************************************************************************
** 函数名称: copyFileWithBuffer
** 功能描述: 通过缓冲区复制文件，适合大文件
** 输　入  : srcPath - 源文件路径, destPath - 目标文件路径
** 输　出  : 成功返回0，失败返回错误代码
***************************************************************************************************/
static int copyFileWithBuffer(const char *srcPath, const char *destPath)
{
    FILE *srcFile, *destFile;
    char buffer[64 * 1024]; // 较小的缓冲区，64KB
    size_t bytesRead;
    int result = 0;
    struct stat st;
    
    // 检查源文件
    if (stat(srcPath, &st) != 0) {
        error("无法获取源文件信息: %s, 错误: %s", srcPath, strerror(errno));
        return -1;
    }
    
    // 获取源文件大小并准备进度报告
    const off_t totalSize = st.st_size;
    off_t totalRead = 0;
    int lastProgress = -1;
    
    logmsg(2, "开始复制文件 %s (%ld 字节) 到 %s", srcPath, (long)totalSize, destPath);
    
    // 使用标准I/O函数打开文件，更可靠但稍慢
    srcFile = fopen(srcPath, "rb");
    if (!srcFile) {
        error("无法打开源文件: %s, 错误: %s", srcPath, strerror(errno));
        return -2;
    }
    
    destFile = fopen(destPath, "wb");
    if (!destFile) {
        error("无法创建目标文件: %s, 错误: %s", destPath, strerror(errno));
        fclose(srcFile);
        return -3;
    }
    
    // 设置较大的缓冲区以提高性能
    setvbuf(srcFile, NULL, _IOFBF, 128 * 1024);
    setvbuf(destFile, NULL, _IOFBF, 128 * 1024);
    
    // 标准I/O函数复制文件内容
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), srcFile)) > 0) {
        // 写入读取的内容
        if (fwrite(buffer, 1, bytesRead, destFile) != bytesRead) {
            error("写入目标文件时出错: %s", strerror(errno));
            result = -4;
            break;
        }
        
        // 累计已读取字节数并报告进度
        totalRead += bytesRead;
        
        if (totalSize > 0) {
            int progress = (int)((totalRead * 100) / totalSize);
            if (progress != lastProgress && progress % 10 == 0) {
                logmsg(2, "文件复制进度: %d%%", progress);
                lastProgress = progress;
            }
        }
    }
    
    // 检查读取错误
    if (ferror(srcFile)) {
        error("读取源文件时出错");
        result = -5;
    }
    
    // 确保所有数据写入磁盘
    fflush(destFile);
    
    // 关闭文件
    fclose(srcFile);
    fclose(destFile);
    
    // 完成复制
    if (result == 0) {
        logmsg(2, "文件复制完成: %ld 字节", (long)totalRead);
    } else {
        // 如果发生错误，删除不完整的目标文件
        unlink(destPath);
        logmsg(1, "文件复制失败，已删除不完整文件");
    }
    
    return result;
}

/***************************************************************************************************
** 函数名称: ensureDirectoryPermissions
** 功能描述: 确保目录具有适当的读写权限
** 输　入  : dirPath - 需要设置权限的目录路径, mode - 要设置的权限模式
** 输　出  : 成功返回0，失败返回非0值
***************************************************************************************************/
static int ensureDirectoryPermissions(const char *dirPath, mode_t mode) {
    /* 先测试目录是否已具有所需权限 */
    if (access(dirPath, R_OK | W_OK) == 0) {
        /* 目录已具有读写权限，无需更改 */
        logmsg(3, "目录已具有读写权限: %s", dirPath);
        return 0;
    }
    
    logmsg(2, "尝试设置目录权限: %s -> 0%o", dirPath, mode);
    
    /* 如果权限不足，尝试设置 */
    if (chmod(dirPath, mode) != 0) {
        error("无法设置目录权限: %s, 错误: %s", dirPath, strerror(errno));
        return -1;
    }
    
    /* 再次检查权限 */
    if (access(dirPath, R_OK | W_OK) != 0) {
        error("设置权限后目录仍不可读写: %s, 错误: %s", dirPath, strerror(errno));
        return -2;
    }
    
    logmsg(2, "已成功设置目录权限: %s", dirPath);
    return 0;
}

/***************************************************************************************************
** 函数名称: build_fs_path
** 功能描述: 构建文件系统路径，统一路径处理方式
** 输　入  : buffer - 输出缓冲区, buffer_size - 缓冲区大小, dir - 目录, filename - 文件名
** 输　出  : 返回buffer指针，方便链式调用
***************************************************************************************************/
static char* build_fs_path(char* buffer, size_t buffer_size, const char* dir, const char* filename) {
    if (!buffer || buffer_size == 0 || !dir) {
        return NULL;
    }
    
    if (filename) {
        snprintf(buffer, buffer_size, "%s/%s", dir, filename);
    } else {
        snprintf(buffer, buffer_size, "%s", dir);
    }
    
    // 确保字符串以null结尾
    buffer[buffer_size - 1] = '\0';
    
    return buffer;
}

/***************************************************************************************************
** 函数名称: initModelDirectories
** 功能描述: 初始化模型目录
** 输　入  : 无
** 输　出  : 成功返回0，失败返回非0值
***************************************************************************************************/
int initModelDirectories(void) {
    int result = 0;
    
    // 使用互斥锁确保线程安全
    pthread_mutex_lock(&g_model_mutex);
    
    // 如果目录已经初始化，直接返回成功
    if (g_dirs_initialized) {
        pthread_mutex_unlock(&g_model_mutex);
        return 0;
    }
    
    // 初始化图片模型目录
    if (access(IMAGE_MODEL_DIR, F_OK) != 0) {
        logmsg(2, "创建图片模型目录: %s", IMAGE_MODEL_DIR);
        if (mkdir_p(IMAGE_MODEL_DIR, 0755) != 0) {
            error("无法创建图片模型目录: %s", IMAGE_MODEL_DIR);
            result = -1;
        }
    } else {
        logmsg(3, "图片模型目录已存在: %s", IMAGE_MODEL_DIR);
    }
    
    // 确保图片模型目录权限
    if (result == 0 && ensureDirectoryPermissions(IMAGE_MODEL_DIR, 0755) != 0) {
        error("无法设置图片模型目录权限: %s", IMAGE_MODEL_DIR);
        result = -2;
    }
    
    // 初始化视频模型目录
    if (access(VIDEO_MODEL_DIR, F_OK) != 0) {
        logmsg(2, "创建视频模型目录: %s", VIDEO_MODEL_DIR);
        if (mkdir_p(VIDEO_MODEL_DIR, 0755) != 0) {
            error("无法创建视频模型目录: %s", VIDEO_MODEL_DIR);
            result = -3;
        }
    } else {
        logmsg(3, "视频模型目录已存在: %s", VIDEO_MODEL_DIR);
    }
    
    // 确保视频模型目录权限
    if (result == 0 && ensureDirectoryPermissions(VIDEO_MODEL_DIR, 0755) != 0) {
        error("无法设置视频模型目录权限: %s", VIDEO_MODEL_DIR);
        result = -4;
    }
    
    if (result == 0) {
        g_dirs_initialized = true;
        logmsg(2, "模型目录初始化成功");
    }
    
    pthread_mutex_unlock(&g_model_mutex);
    return result;
}

/***************************************************************************************************
** 函数名称: uploadModel_fun
** 功能描述: 处理模型上传请求
** 输　入  : wp - GoAhead web请求结构体
** 输　出  : 无
***************************************************************************************************/
void uploadModel_fun(Webs *wp) {
    WebsKey *s;
    WebsUpload *up;
    char *upfile;
    char response[MAX_RESPONSE_SIZE];
    int jsonLength = 0;
    struct stat fileStat;
    int modelType = MODEL_TYPE_IMAGE; // 默认为图片模型
    const char *modelTypeParam = NULL;
    const char *modelDir = NULL;
    
    // 设置HTTP响应头
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    // 确保模型目录存在
    if (initModelDirectories() != 0) {
        logmsg(1, "初始化模型目录失败");
    }
    
    memset(response, 0, MAX_RESPONSE_SIZE);
    jsonLength += sprintf(response + jsonLength, "{");
    
    // 获取模型类型参数
    modelTypeParam = websGetVar(wp, "modelType", NULL);
    if (modelTypeParam) {
        modelType = atoi(modelTypeParam);
        
        // 验证模型类型值
        if (modelType != MODEL_TYPE_IMAGE && modelType != MODEL_TYPE_VIDEO) {
            modelType = MODEL_TYPE_IMAGE; // 如果无效，默认使用图片模型类型
        }
    }
    
    // 根据模型类型确定目标目录
    if (modelType == MODEL_TYPE_VIDEO) {
        modelDir = VIDEO_MODEL_DIR;
        logmsg(2, "上传视频处理模型");
    } else {
        modelDir = IMAGE_MODEL_DIR;
        logmsg(2, "上传图片处理模型");
    }
    
    // 检查是否为POST请求
    if (scaselessmatch(wp->method, "POST")) {
        // 遍历上传的文件
        for (s = hashFirst(wp->files); s; s = hashNext(wp->files, s)) {
            up = s->content.value.symbol;
            
            // 检查文件大小
            if (stat(up->filename, &fileStat) == 0) {
                logmsg(2, "上传模型文件大小: %ld 字节", (long)fileStat.st_size);
            }
            
            // 检查文件名是否包含非法字符
            if (strpbrk(up->clientFilename, "\\/:*?\"<>|") != NULL) {
                logmsg(1, "文件名包含非法字符: %s", up->clientFilename);
                jsonLength += sprintf(response + jsonLength, 
                    "\"resp_code\": -1, \"resp_msg\": \"文件名包含非法字符\", \"resp_target\": \"null\", \"datas\": {}");
                break;
            }
            
            // 验证文件扩展名
            char *ext = strrchr(up->clientFilename, '.');
            if (!ext || strcasecmp(ext, VALID_MODEL_EXT) != 0) {
                logmsg(1, "不支持的模型文件类型: %s", up->clientFilename);
                jsonLength += sprintf(response + jsonLength, 
                    "\"resp_code\": -1, \"resp_msg\": \"不支持的模型文件类型，仅支持%s文件\", \"resp_target\": \"null\", \"datas\": {}",
                    VALID_MODEL_EXT);
                break;
            }
            
            // 构建目标文件路径
            char dest_path[MAX_PATH_LENGTH];
            build_fs_path(dest_path, sizeof(dest_path), modelDir, up->clientFilename);
            upfile = sclone(dest_path); // 使用sclone创建可释放的副本
            
            // 如果目标文件已存在，先删除
            if (access(upfile, F_OK) != -1) {
                if (unlink(upfile) != 0) {
                    logmsg(1, "删除已存在的文件失败: %s, 错误: %s", upfile, strerror(errno));
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": -1, \"resp_msg\": \"无法覆盖现有文件\", \"resp_target\": \"null\", \"datas\": {}");
                    wfree(upfile);
                    break;
                }
            }
            
            // 使用缓冲区复制提高大文件性能
            int copyResult = copyFileWithBuffer(up->filename, upfile);
            
            if (copyResult != 0) {
                error("复制上传文件失败: %s -> %s, 错误码: %d", up->filename, upfile, copyResult);
                jsonLength += sprintf(response + jsonLength, 
                    "\"resp_code\": -1, \"resp_msg\": \"上传失败: 文件复制错误\", \"resp_target\": \"null\", \"datas\": {}");
            } else {
                // 删除临时文件
                if (unlink(up->filename) != 0) {
                    logmsg(1, "删除临时文件失败: %s, 错误: %s", up->filename, strerror(errno));
                    // 不影响主要流程，继续执行
                }
                
                // 验证复制的文件
                if (stat(upfile, &fileStat) != 0 || fileStat.st_size == 0) {
                    error("上传文件验证失败: %s, 大小: %ld", upfile, 
                          stat(upfile, &fileStat) == 0 ? (long)fileStat.st_size : -1);
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": -1, \"resp_msg\": \"上传失败: 文件验证错误\", \"resp_target\": \"null\", \"datas\": {}");
                } else {
                    logmsg(2, "模型文件上传成功: %s, 大小: %ld 字节", up->clientFilename, (long)fileStat.st_size);
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": 0, \"resp_msg\": \"上传成功\", \"resp_target\": \"null\", \"datas\": {\"filename\": \"%s\", \"modelType\": %d, \"size\": %ld}", 
                        up->clientFilename, modelType, (long)fileStat.st_size);
                }
            }
            
            wfree(upfile);
            break; // 只处理第一个文件
        }
    } else {
        jsonLength += sprintf(response + jsonLength, "\"resp_code\": -1, \"resp_msg\": \"请使用POST方法上传\", \"resp_target\": \"null\", \"datas\": {}");
    }
    
    jsonLength += sprintf(response + jsonLength, "}");
    websWrite(wp, "%s", response);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: deleteModel_fun
** 功能描述: 处理模型删除请求
** 输　入  : wp - GoAhead web请求结构体
** 输　出  : 无
***************************************************************************************************/
void deleteModel_fun(Webs *wp) {
    char *modelName = NULL;                /* 模型名称参数 */
    int modelType = 0;                     /* 模型类型 */
    cJSON *response = NULL;                /* JSON响应对象 */
    cJSON *datas = NULL;                   /* JSON数据对象 */
    char *jsonString = NULL;               /* JSON字符串 */
    char modelPath[512];                   /* 模型文件路径 */
    const char *modelDir = NULL;           /* 模型目录 */
    char *requestBody = NULL;              /* 请求体 */
    cJSON *requestJson = NULL;             /* 请求JSON对象 */
    cJSON *modelNameObj = NULL;            /* 模型名称JSON对象 */
    cJSON *modelTypeObj = NULL;            /* 模型类型JSON对象 */
    bool paramsValid = false;              /* 参数验证标志 */

    /* 设置HTTP响应头 */
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);

    /* 创建JSON响应 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();

    /* 记录HTTP请求方法和内容类型 */
    logmsg(2, "deleteModel_fun: 请求方法: %s, 内容类型: %s, 内容长度: %d, 标志: %d", 
           wp->method, wp->contentType, (int)wp->rxLen, wp->flags);
    
    /* 尝试从GET参数中获取modelName和modelType */
    modelName = (char *)websGetVar(wp, "modelName", NULL);
    const char *typeStr = websGetVar(wp, "modelType", NULL);
    logmsg(2, "deleteModel_fun: GET参数 - modelName: %s, modelType: %s", 
           modelName ? modelName : "NULL", typeStr ? typeStr : "NULL");
    
    if (modelName && typeStr) {
        modelType = atoi(typeStr);
        if (modelType == MODEL_TYPE_IMAGE || modelType == MODEL_TYPE_VIDEO) {
            paramsValid = true;
            logmsg(2, "deleteModel_fun: 从GET参数中获取有效参数");
        }
    } 
    /* 如果GET参数不可用，尝试解析POST JSON数据或表单数据 */
    else if (wp->rxLen > 0) {
        logmsg(2, "deleteModel_fun: 尝试解析POST请求体, rxLen=%d, rxRemaining=%d", 
               (int)wp->rxLen, (int)wp->rxRemaining);
        
        /* 确保我们处理正确的长度 - 使用文档中的总内容长度而不是剩余长度 */
        int contentLen = wp->rxLen;
        requestBody = malloc(contentLen + 1);
        if (requestBody) {
            memcpy(requestBody, wp->input.servp, contentLen);
            requestBody[contentLen] = '\0';
            logmsg(2, "deleteModel_fun: 请求体地址: %p, input.servp: %p", requestBody, wp->input.servp);
            
            /* 记录收到的原始数据用于调试 */
            logmsg(2, "deleteModel_fun: 收到的原始数据: %s", requestBody);
            
            /* 检查是否是JSON请求 */
            if (wp->contentType && strstr(wp->contentType, "application/json")) {
                logmsg(2, "deleteModel_fun: 检测到JSON请求，内容: %s", requestBody);
                
                /* 逐字节打印请求体，以便检查UTF-8和特殊字符 */
                logmsg(2, "deleteModel_fun: 请求体字节内容: ");
                for (int i = 0; i < contentLen && i < 100; i++) {
                    logmsg(2, "  字节[%d]: %02X (%c)", i, (unsigned char)requestBody[i], 
                          isprint(requestBody[i]) ? requestBody[i] : '.');
                }
                
                /* 解析JSON */
                requestJson = cJSON_Parse(requestBody);
                if (requestJson) {
                    char *jsonDump = cJSON_Print(requestJson);
                    logmsg(2, "deleteModel_fun: JSON解析成功, 内容: %s", jsonDump);
                    
                    /* 获取modelName参数 */
                    modelNameObj = cJSON_GetObjectItem(requestJson, "modelName");
                    if (modelNameObj) {
                        logmsg(2, "deleteModel_fun: 找到modelName项, 类型: %d", modelNameObj->type);
                        if (cJSON_IsString(modelNameObj) && modelNameObj->valuestring) {
                            modelName = modelNameObj->valuestring;
                            logmsg(2, "deleteModel_fun: 从JSON中获取modelName: %s", modelName);
                        } else {
                            logmsg(2, "deleteModel_fun: modelName不是字符串类型");
                        }
                        
                        /* 获取modelType参数 */
                        modelTypeObj = cJSON_GetObjectItem(requestJson, "modelType");
                        if (modelTypeObj) {
                            logmsg(2, "deleteModel_fun: 找到modelType项, 类型: %d", modelTypeObj->type);
                            if (cJSON_IsNumber(modelTypeObj)) {
                                modelType = modelTypeObj->valueint;
                                paramsValid = true;
                                logmsg(2, "deleteModel_fun: 从JSON中获取modelType: %d", modelType);
                            } else {
                                logmsg(2, "deleteModel_fun: modelType不是数字类型");
                            }
                        } else {
                            logmsg(2, "deleteModel_fun: JSON中不存在modelType项");
                        }
                    } else {
                        logmsg(2, "deleteModel_fun: JSON中不存在modelName项");
                    }
                    free(jsonDump);
                } else {
                    /* 获取cJSON错误信息 */
                    const char *error_ptr = cJSON_GetErrorPtr();
                    if (error_ptr != NULL) {
                        logmsg(2, "deleteModel_fun: JSON解析错误，位置: %s", error_ptr);
                    } else {
                        logmsg(2, "deleteModel_fun: JSON解析失败，未知错误");
                    }
                }
            } 
            /* 检查是否是表单请求，尝试从表单参数中获取 */
            else {
                logmsg(2, "deleteModel_fun: 尝试手动解析表单请求");
                
                /* 手动解析表单数据 */
                char *decodedBody = malloc(contentLen + 1);
                if (decodedBody) {
                    char *pair, *next, *key, *value, *saveptr1, *saveptr2;
                    
                    /* 创建请求体的副本并解码URL编码 */
                    websDecodeUrl(decodedBody, requestBody, contentLen);
                    logmsg(2, "deleteModel_fun: 解码后的表单数据: %s", decodedBody);
                    
                    /* 按键值对分割字符串 */
                    for (pair = strtok_r(decodedBody, "&", &saveptr1); pair; pair = strtok_r(NULL, "&", &saveptr1)) {
                        key = strtok_r(pair, "=", &saveptr2);
                        value = strtok_r(NULL, "=", &saveptr2);
                        
                        if (key && value) {
                            logmsg(2, "deleteModel_fun: 解析到表单参数: %s=%s", key, value);
                            
                            if (strcmp(key, "modelName") == 0) {
                                modelName = sclone(value); /* 使用sclone创建可释放的副本 */
                            } else if (strcmp(key, "modelType") == 0) {
                                modelType = atoi(value);
                            }
                        }
                    }
                    
                    free(decodedBody);
                    
                    if (modelName && (modelType == MODEL_TYPE_IMAGE || modelType == MODEL_TYPE_VIDEO)) {
                        paramsValid = true;
                        logmsg(2, "deleteModel_fun: 从表单参数中获取有效参数 - modelName: %s, modelType: %d", 
                               modelName, modelType);
                    }
                }
            }
        }
    }

    /* 验证参数 */
    if (!paramsValid || !modelName || !*modelName) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "缺少必要参数");
        goto done;
    }

    /* 验证模型类型 */
    if (modelType != MODEL_TYPE_IMAGE && modelType != MODEL_TYPE_VIDEO) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "无效的模型类型");
        goto done;
    }

    /* 根据模型类型确定目录 */
    modelDir = (modelType == MODEL_TYPE_IMAGE) ? IMAGE_MODEL_DIR : VIDEO_MODEL_DIR;

    /* 安全性检查 - 防止路径遍历攻击 */
    if (strstr(modelName, "..") || strchr(modelName, '/') || strchr(modelName, '\\')) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "非法的模型名称");
        goto done;
    }

    /* 构建模型文件路径 */
    snprintf(modelPath, sizeof(modelPath), "%s/%s", modelDir, modelName);

    /* 检查模型文件是否存在 */
    if (access(modelPath, F_OK) == -1) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "模型文件不存在");
        goto done;
    }

    /* 执行删除操作 */
    if (unlink(modelPath) != 0) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "删除文件失败");
        cJSON_AddStringToObject(datas, "error", strerror(errno));
        logmsg(2, "deleteModel_fun: 删除文件失败: %s, 错误: %s", modelPath, strerror(errno));
    } else {
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "删除成功");
        cJSON_AddStringToObject(datas, "modelName", modelName);
        
        /* 记录删除日志 */
        logmsg(2, "deleteModel_fun: 已删除模型: %s, 类型: %s", 
              modelName, modelType == MODEL_TYPE_IMAGE ? "图像" : "视频");
    }

done:
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    /* 释放资源 */
    if (requestJson) {
        cJSON_Delete(requestJson);
    }
    if (requestBody) {
        free(requestBody);
    }
    
    /* 如果modelName是从表单解析中动态分配的，则需要释放 */
    if (modelName && paramsValid && (!modelNameObj || !cJSON_IsString(modelNameObj))) {
        wfree(modelName);
    }
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
} 