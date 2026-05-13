/********************************* Includes ***********************************/

#include    "goahead.h"        /* GoAhead核心头文件 */
#include    "videoprocess.h"   /* 当前模块的头文件 */
#include    "cJSON.h"          /* JSON解析库 */
#include    <stdio.h>          /* 标准输入输出函数 */
#include    <stdlib.h>         /* 标准库函数 */
#include    <string.h>         /* 字符串操作函数 */
#include    <ctype.h>          /* 字符处理函数 */
#include    <sys/types.h>      /* 系统类型定义 */
#include    <sys/stat.h>       /* 文件状态相关 */
#include    <dirent.h>         /* 目录操作函数 */
#include    <unistd.h>         /* Unix标准函数 */
#include    <pthread.h>        /* POSIX线程库 */
#include    <errno.h>          /* 错误码定义 */
#include    <time.h>           /* 时间操作相关函数 */
#include    <fcntl.h>          /* 文件控制操作 */
#include    <stdbool.h>        /* 布尔类型 */

/********************************* Defines ************************************/

#define MAX_VIDEOS 20                     /* 定义最大处理视频数量 */
#define VIDEO_DIR "web/videos"            /* 定义上传视频存储目录 */
#define PROCESSED_DIR "web/processed_videos" /* 定义处理后视频存储目录 */
#define VIDEO_URL_PREFIX "/videos"        /* 定义原始视频URL前缀 */
#define PROCESSED_URL_PREFIX "/processed_videos" /* 定义处理后视频URL前缀 */
#define MODEL_DIR "web/model/video"         /* 定义模型存储目录 */
#define VIDEO_PROGRESS_FILE "/tmp/video_progress.json" /* 视频处理进度文件 */
#define DEFAULT_MODEL ""                  /* 默认模型运行时自动解析 */
#define MAX_CMD_LEN 1024                  /* 最大命令长度 */
#define MAX_RESPONSE_SIZE 1024             /* 最大响应大小 */
#define MAX_PATH_LENGTH 512               /* 最大路径长度 */

/********************************* Globals ************************************/

typedef struct {                       /* 定义视频信息结构体 */
    char filename[256];                /* 完整文件路径 */
    char clientFilename[256];          /* 客户端原始文件名 */
    bool processed;                    /* 处理状态标志 */
} VideoInfo;

static VideoInfo g_videos[MAX_VIDEOS]; /* 视频信息数组，存储所有待处理和已处理的视频 */
static int g_videoCount = 0;           /* 当前视频总数 */
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER; /* 互斥锁，保护共享数据 */
char g_modelPath[256] = DEFAULT_MODEL; /* 当前使用的模型路径，默认为yolov5s.rknn */

/********************************* Forwards ***********************************/

static void initVideoDirs(void);             /* 初始化视频存储目录函数声明 */
static void clearVideoList(void);            /* 清空视频列表函数声明 */
static bool processVideoFile(const char* filename); /* 处理单个视频文件函数声明 */
static void url_decode(char *str);           /* URL解码函数声明 */
static int remove_dir_contents(const char *path, bool remove_hidden_files); /* 递归删除目录内容函数声明 */
static int mkdir_p(const char *path, mode_t mode); /* 递归创建目录函数声明 */
static char* build_fs_path(char* buffer, size_t buffer_size, const char* dir, const char* filename); /* 构建文件系统路径函数声明 */
static char* build_url_path(char* buffer, size_t buffer_size, const char* prefix, const char* filename); /* 构建URL路径函数声明 */
static int ensureVideoDirectoryPermissions(const char *dirPath, mode_t mode); /* 确保目录具有适当权限函数声明 */
static bool isVideoFile(const char *filename);
static bool isVideoModelFile(const char *filename);
static bool resolveCurrentVideoModel(char *buffer, size_t buffer_size, bool updateGlobal);
static void removeProcessedVideoFor(const char *filename);

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
** 函数名称: remove_dir_contents
** 功能描述: 递归删除目录中的内容（文件和子目录）
** 输　入  : path - 目录路径, remove_hidden_files - 是否删除隐藏文件
** 输　出  : 成功返回0，失败返回错误数量
***************************************************************************************************/
static int remove_dir_contents(const char *path, bool remove_hidden_files) {
    DIR *dir;
    struct dirent *entry;
    char full_path[MAX_PATH_LENGTH];
    struct stat st;
    int error_count = 0;

    /* 打开目录 */
    dir = opendir(path);
    if (dir == NULL) {
        return 1; /* 打开目录失败 */
    }

    /* 遍历目录中的所有条目 */
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过. 和 .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* 如果不删除隐藏文件，则跳过以.开头的文件 */
        if (!remove_hidden_files && entry->d_name[0] == '.') {
            continue;
        }

        /* 构建完整路径 */
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        /* 获取文件信息 */
        if (stat(full_path, &st) != 0) {
            error_count++;
            continue;
        }

        /* 如果是目录，首先递归删除其内容，然后删除目录本身 */
        if (S_ISDIR(st.st_mode)) {
            if (remove_dir_contents(full_path, remove_hidden_files) != 0) {
                error_count++;
            }
            
            /* 对于tmp_开头的目录，直接删除目录本身 */
            if (strncmp(entry->d_name, "tmp_", 4) == 0) {
                if (rmdir(full_path) != 0) {
                    error_count++;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            /* 删除常规文件 */
            if (unlink(full_path) != 0) {
                error_count++;
            }
        }
    }

    closedir(dir);
    return error_count;
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
** 函数名称: initVideoDirs
** 功能描述: 初始化视频存储目录
** 输　入  : 无
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
static void initVideoDirs(void)
{
    static int dirs_initialized = 0;           /* 目录初始化标志 */
    char video_dir_path[MAX_PATH_LENGTH];      /* 视频目录路径缓冲区 */
    char processed_dir_path[MAX_PATH_LENGTH];  /* 处理后视频目录路径缓冲区 */
    
    /* 如果目录已经初始化过，直接返回 */
    if (dirs_initialized) {
        return;
    }

    /* 构建目录路径 */
    build_fs_path(video_dir_path, sizeof(video_dir_path), VIDEO_DIR, NULL);
    build_fs_path(processed_dir_path, sizeof(processed_dir_path), PROCESSED_DIR, NULL);

    /* 检查原始视频目录是否存在 */
    if (access(video_dir_path, F_OK) != 0) {
        /* 目录不存在，创建 */
        printf("创建原始视频目录: %s\n", video_dir_path);
        mkdir_p(video_dir_path, 0755);
    } else {
        printf("原始视频目录已存在: %s\n", video_dir_path);
    }

    /* 检查处理后视频目录是否存在 */
    if (access(processed_dir_path, F_OK) != 0) {
        /* 目录不存在，创建 */
        printf("创建处理后视频目录: %s\n", processed_dir_path);
        mkdir_p(processed_dir_path, 0755);
    } else {
        printf("处理后视频目录已存在: %s\n", processed_dir_path);
    }
    
    /* 标记目录已初始化 */
    dirs_initialized = 1;
}

/***************************************************************************************************
** 函数名称: clearVideoList
** 功能描述: 清空视频列表
** 输　入  : 无
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
static void clearVideoList(void)
{
    pthread_mutex_lock(&g_mutex);            /* 获取互斥锁，保护共享数据 */
    g_videoCount = 0;                        /* 重置视频计数为0 */
    memset(g_videos, 0, sizeof(g_videos));   /* 清空视频信息数组 */
    pthread_mutex_unlock(&g_mutex);          /* 释放互斥锁 */
}

static bool isVideoFile(const char *filename)
{
    char *ext;

    if (!filename || !*filename) {
        return false;
    }
    ext = strrchr(filename, '.');
    return ext && (scaselesscmp(ext, ".mp4") == 0 ||
        scaselesscmp(ext, ".avi") == 0 ||
        scaselesscmp(ext, ".mov") == 0 ||
        scaselesscmp(ext, ".mkv") == 0 ||
        scaselesscmp(ext, ".webm") == 0 ||
        scaselesscmp(ext, ".flv") == 0);
}

static bool isVideoModelFile(const char *filename)
{
    char *ext;

    if (!filename || !*filename) {
        return false;
    }
    ext = strrchr(filename, '.');
    return ext && scaselesscmp(ext, ".rknn") == 0;
}

static bool resolveCurrentVideoModel(char *buffer, size_t buffer_size, bool updateGlobal)
{
    DIR             *dir;
    struct dirent   *entry;
    char            firstModel[256];
    char            selectedModel[256];
    bool            foundCurrent;

    if (!buffer || buffer_size == 0) {
        return false;
    }
    buffer[0] = '\0';
    firstModel[0] = '\0';
    selectedModel[0] = '\0';
    foundCurrent = false;

    dir = opendir(MODEL_DIR);
    if (!dir) {
        logmsg(1, "无法打开视频模型目录: %s", MODEL_DIR);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!isVideoModelFile(entry->d_name)) {
            continue;
        }
        if (firstModel[0] == '\0' || strcmp(entry->d_name, firstModel) < 0) {
            strncpy(firstModel, entry->d_name, sizeof(firstModel) - 1);
            firstModel[sizeof(firstModel) - 1] = '\0';
        }
        if (g_modelPath[0] != '\0' && strcmp(entry->d_name, g_modelPath) == 0) {
            strncpy(selectedModel, entry->d_name, sizeof(selectedModel) - 1);
            selectedModel[sizeof(selectedModel) - 1] = '\0';
            foundCurrent = true;
        }
    }
    closedir(dir);

    if (!foundCurrent && firstModel[0] != '\0') {
        strncpy(selectedModel, firstModel, sizeof(selectedModel) - 1);
        selectedModel[sizeof(selectedModel) - 1] = '\0';
    }
    if (selectedModel[0] == '\0') {
        return false;
    }

    strncpy(buffer, selectedModel, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';

    if (updateGlobal) {
        pthread_mutex_lock(&g_mutex);
        strncpy(g_modelPath, selectedModel, sizeof(g_modelPath) - 1);
        g_modelPath[sizeof(g_modelPath) - 1] = '\0';
        pthread_mutex_unlock(&g_mutex);
    }
    return true;
}

static void removeProcessedVideoFor(const char *filename)
{
    char processedPath[MAX_PATH_LENGTH];

    if (!filename || !*filename) {
        return;
    }
    build_fs_path(processedPath, sizeof(processedPath), PROCESSED_DIR, filename);
    if (access(processedPath, F_OK) == 0 && unlink(processedPath) != 0) {
        logmsg(1, "删除旧处理视频失败: %s, 错误: %s", processedPath, strerror(errno));
    }
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
** 函数名称: uploadVideo_fun
** 功能描述: 处理视频上传请求
** 输　入  : wp 
** 输　出  : 无
***************************************************************************************************/
void uploadVideo_fun(Webs *wp)
{
    WebsKey *s;
    WebsUpload *up;
    char *upfile;
    char response[MAX_RESPONSE_SIZE];
    int jsonLength = 0;
    struct stat fileStat;
    
    /* 设置HTTP响应头 */
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    /* 确保视频目录存在 - 使用静态变量确保只初始化一次 */
    static bool dirsInitialized = false;
    if (!dirsInitialized) {
        initVideoDirs();
        dirsInitialized = true;
    }
    
    memset(response, 0, MAX_RESPONSE_SIZE);
    jsonLength += sprintf(response + jsonLength, "{");
    
    /* 检查是否为POST请求 */
    if (scaselessmatch(wp->method, "POST")) {
        for (s = hashFirst(wp->files); s; s = hashNext(wp->files, s)) {
            up = s->content.value.symbol;

            if (!isVideoFile(up->clientFilename)) {
                jsonLength += sprintf(response + jsonLength,
                    "\"resp_code\": -1, \"resp_msg\": \"不支持的视频类型\", \"resp_target\": \"null\", \"datas\": {}");
                break;
            }
            
            /* 检查文件大小 */
            if (stat(up->filename, &fileStat) == 0) {
                logmsg(2, "上传文件大小: %ld 字节", (long)fileStat.st_size);
            }
            
            /* 检查文件名是否包含非法字符 */
            if (strpbrk(up->clientFilename, "\\/:*?\"<>|") != NULL) {
                logmsg(1, "文件名包含非法字符: %s", up->clientFilename);
                jsonLength += sprintf(response + jsonLength, 
                    "\"resp_code\": -1, \"resp_msg\": \"文件名包含非法字符\", \"resp_target\": \"null\", \"datas\": {}");
                break;
            }
            
            /* 构建目标文件路径 */
            char dest_path[MAX_PATH_LENGTH];
            build_fs_path(dest_path, sizeof(dest_path), VIDEO_DIR, up->clientFilename);
            upfile = sclone(dest_path); /* 使用sclone创建可释放的副本 */
            
            /* 如果目标文件已存在，先删除 */
            if (access(upfile, F_OK) != -1) {
                if (unlink(upfile) != 0) {
                    logmsg(1, "删除已存在的文件失败: %s, 错误: %s", upfile, strerror(errno));
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": -1, \"resp_msg\": \"无法覆盖现有文件\", \"resp_target\": \"null\", \"datas\": {}");
                    wfree(upfile);
                    break;
                }
            }
            
            /* 使用缓冲区复制提高大文件性能 */
            int copyResult = copyFileWithBuffer(up->filename, upfile);
            
            if (copyResult != 0) {
                error("复制上传文件失败: %s -> %s, 错误码: %d", up->filename, upfile, copyResult);
                jsonLength += sprintf(response + jsonLength, 
                    "\"resp_code\": -1, \"resp_msg\": \"上传失败: 文件复制错误\", \"resp_target\": \"null\", \"datas\": {}");
            } else {
                /* 删除临时文件 */
                if (unlink(up->filename) != 0) {
                    logmsg(1, "删除临时文件失败: %s, 错误: %s", up->filename, strerror(errno));
                    // 不影响主要流程，继续执行
                }
                removeProcessedVideoFor(up->clientFilename);
                
                /* 验证复制的文件 */
                if (stat(upfile, &fileStat) != 0 || fileStat.st_size == 0) {
                    error("上传文件验证失败: %s, 大小: %ld", upfile, 
                          stat(upfile, &fileStat) == 0 ? (long)fileStat.st_size : -1);
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": -1, \"resp_msg\": \"上传失败: 文件验证错误\", \"resp_target\": \"null\", \"datas\": {}");
                } else {
                    logmsg(2, "文件上传成功: %s, 大小: %ld 字节", up->clientFilename, (long)fileStat.st_size);
                    /* 构建URL路径 */
                    char url_path[MAX_PATH_LENGTH];
                    build_url_path(url_path, sizeof(url_path), VIDEO_URL_PREFIX, up->clientFilename);
                    
                    jsonLength += sprintf(response + jsonLength, 
                        "\"resp_code\": 0, \"resp_msg\": \"上传成功\", \"resp_target\": \"null\", \"datas\": {\"filename\": \"%s\", \"url\": \"%s\", \"size\": %ld}", 
                        up->clientFilename, url_path, (long)fileStat.st_size);
                }
            }
            
            wfree(upfile);
            break; /* 只处理第一个文件 */
        }
    } else {
        jsonLength += sprintf(response + jsonLength, "\"resp_code\": -1, \"resp_msg\": \"请使用POST方法上传\", \"resp_target\": \"null\", \"datas\": {}");
    }
    
    jsonLength += sprintf(response + jsonLength, "}");
    websWrite(wp, "%s", response);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: processVideo_fun
** 功能描述: 处理单个视频
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void processVideo_fun(Webs * wp)
{
    cchar *videoFilename = NULL;             /* 视频文件名参数 */
    cJSON *response = NULL;                  /* JSON响应对象 */
    cJSON *datas = NULL;                     /* JSON数据对象 */
    char *jsonString = NULL;                 /* JSON字符串 */
    bool result = false;                     /* 处理结果 */
    cJSON *requestJson = NULL;               /* 请求JSON对象 */
    char *requestBody = NULL;                /* 请求体内容 */
    char *uri, *query, *key, *value;         /* URI 解析临时变量 */
    char decodedFilename[256];               /* 解码后的文件名 */
    char srcPath[MAX_PATH_LENGTH];           /* 源视频路径 */
    char processedPath[MAX_PATH_LENGTH];     /* 处理后视频保存路径 */
    struct stat st;                          /* 文件状态结构 */
    
    websSetStatus(wp, 200);                  /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);             /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);                 /* 结束HTTP头部写入 */
    
    response = cJSON_CreateObject();         /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();            /* 创建数据对象 */
    
    /* 从URL查询字符串获取参数（如 /action/processVideo?filename=xxx.mp4） */
    uri = sclone(wp->url);
    if ((query = strchr(uri, '?')) != NULL) {
        *query++ = '\0';
        
        while (query && *query) {
            key = stok(query, "&", &query);
            if ((value = strchr(key, '=')) != NULL) {
                *value++ = '\0';
                if (strcmp(key, "filename") == 0 && *value) {
                    videoFilename = value;
                    
                    /* 复制并解码文件名 */
                    strncpy(decodedFilename, videoFilename, sizeof(decodedFilename) - 1);
                    decodedFilename[sizeof(decodedFilename) - 1] = '\0';
                    url_decode(decodedFilename);
                    videoFilename = decodedFilename;
                    break;
                }
            }
        }
    }
    wfree(uri);
    
    /* 如果URL中没有找到，尝试从查询参数获取 */
    if (!videoFilename || !*videoFilename) {
        videoFilename = websGetVar(wp, "filename", NULL);
        if (videoFilename && *videoFilename) {
            /* 复制并解码文件名 */
            strncpy(decodedFilename, videoFilename, sizeof(decodedFilename) - 1);
            decodedFilename[sizeof(decodedFilename) - 1] = '\0';
            url_decode(decodedFilename);
            videoFilename = decodedFilename;
        }
    }
    
    /* 如果仍然没有找到，尝试从JSON请求体获取 */
    if ((!videoFilename || !*videoFilename) && wp->rxRemaining > 0) {
        requestBody = malloc(wp->rxRemaining + 1);
        if (requestBody) {
            memcpy(requestBody, wp->input.servp, wp->rxRemaining);
            requestBody[wp->rxRemaining] = '\0';
            
            /* 尝试解析JSON */
            requestJson = cJSON_Parse(requestBody);
            if (requestJson) {
                cJSON *filenameObj = cJSON_GetObjectItem(requestJson, "filename");
                if (filenameObj && cJSON_IsString(filenameObj) && filenameObj->valuestring) {
                    videoFilename = filenameObj->valuestring;
                    
                    /* 复制并解码文件名 */
                    strncpy(decodedFilename, videoFilename, sizeof(decodedFilename) - 1);
                    decodedFilename[sizeof(decodedFilename) - 1] = '\0';
                    url_decode(decodedFilename);
                    videoFilename = decodedFilename;
                }
            }
        }
    }
    
    /* 校验参数 */
    if (!videoFilename || !*videoFilename) {
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "缺少文件名参数");
    } else {
        /* 构建源文件和处理后文件路径 */
        build_fs_path(srcPath, sizeof(srcPath), VIDEO_DIR, videoFilename);
        build_fs_path(processedPath, sizeof(processedPath), PROCESSED_DIR, videoFilename);
        
        /* 检查视频文件是否存在 */
        if (access(srcPath, F_OK) != 0) {
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "视频文件不存在");
            goto done;
        }
        
        /* 切换模型后需要允许同一视频重新处理，因此始终删除旧结果 */
        if (access(processedPath, F_OK) == 0) {
            remove(processedPath);
        }

        result = processVideoFile(videoFilename);
        
        if (result) {
            cJSON_AddNumberToObject(response, "resp_code", 0);
            cJSON_AddStringToObject(response, "resp_msg", "处理成功");
            char processed_url[MAX_PATH_LENGTH];
            build_url_path(processed_url, sizeof(processed_url), PROCESSED_URL_PREFIX, videoFilename);
            
            cJSON_AddStringToObject(datas, "processedFile", videoFilename);
            cJSON_AddStringToObject(datas, "processedUrl", processed_url);
            
            /* 添加使用的模型信息 */
            pthread_mutex_lock(&g_mutex);
            cJSON_AddStringToObject(datas, "usedModel", g_modelPath);
            pthread_mutex_unlock(&g_mutex);
        } else {
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "处理失败");
        }
    }
    
done:
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);     /* 将JSON对象转换为字符串 */
    websWrite(wp, "%s", jsonString);        /* 将JSON字符串写入响应 */
    
    /* 清理资源 */
    cJSON_Delete(response);                 /* 释放JSON对象 */
    free(jsonString);                       /* 释放JSON字符串 */
    
    if (requestJson) {
        cJSON_Delete(requestJson);          /* 释放请求JSON对象 */
    }
    if (requestBody) {
        free(requestBody);                  /* 释放请求体内存 */
    }
    
    websDone(wp);                           /* 完成响应处理 */
}

/***************************************************************************************************
** 函数名称: getVideoList_fun
** 功能描述: 获取视频列表功能
** 输　入  : wp
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void getVideoList_fun(Webs * wp)
{
    cJSON *response = NULL;       /* JSON响应对象 */
    cJSON *datas = NULL;          /* JSON数据对象 */
    cJSON *videoArray = NULL;     /* 视频数组 */
    char *jsonString = NULL;      /* JSON字符串 */
    DIR *dir;                    /* 目录流指针 */
    struct dirent *entry;        /* 目录项结构指针 */
    struct stat st;              /* 文件状态结构 */
    char videoPath[512];         /* 存储视频完整路径 */
    char processedPath[512];     /* 存储处理后视频完整路径 */
    int videoCount = 0;          /* 视频计数 */
    int processedCount = 0;      /* 已处理视频计数 */
    
    websSetStatus(wp, 200);       /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);  /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);      /* 结束HTTP头部写入 */
    
    /* 创建JSON响应对象 */
    response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "resp_code", 0);
    cJSON_AddStringToObject(response, "resp_msg", "成功获取视频列表");
    cJSON_AddStringToObject(response, "resp_target", "null");
    
    /* 创建数据对象 */
    datas = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "datas", datas);
    
    /* 创建视频数组 */
    videoArray = cJSON_CreateArray();
    cJSON_AddItemToObject(datas, "videos", videoArray);
    
    /* 确保目录存在 */
    initVideoDirs();
    
    printf("扫描视频目录...\n");
    
    /* 扫描视频目录 */
    dir = opendir(VIDEO_DIR);
    if (dir) {
        /* 遍历目录下的所有视频文件 */
        while ((entry = readdir(dir)) != NULL) {
            /* 忽略.和..目录 */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            /* 构建完整路径 */
            build_fs_path(videoPath, sizeof(videoPath), VIDEO_DIR, entry->d_name);
            printf("检查文件: %s\n", videoPath);
            
            /* 使用stat检查是否为常规文件 */
            if (stat(videoPath, &st) == 0 && S_ISREG(st.st_mode)) {
                /* 只处理视频格式文件 */
                char *ext = strrchr(entry->d_name, '.');
                if (isVideoFile(entry->d_name)) {
                    
                    printf("找到视频文件: %s\n", entry->d_name);
                    
                    cJSON *videoObj = cJSON_CreateObject();
                    char original_url[MAX_PATH_LENGTH];
                    build_url_path(original_url, sizeof(original_url), VIDEO_URL_PREFIX, entry->d_name);
                    
                    cJSON_AddStringToObject(videoObj, "name", entry->d_name);
                    cJSON_AddStringToObject(videoObj, "originalUrl", original_url);
                    
                    /* 检查该视频是否已经处理过 */
                    build_fs_path(processedPath, sizeof(processedPath), PROCESSED_DIR, entry->d_name);
                    if (access(processedPath, F_OK) != -1) {
                        struct stat proc_st;
                        if (stat(processedPath, &proc_st) == 0 && proc_st.st_size > 0) {
                            printf("已处理的视频: %s\n", entry->d_name);
                            char processed_url[MAX_PATH_LENGTH];
                            build_url_path(processed_url, sizeof(processed_url), PROCESSED_URL_PREFIX, entry->d_name);
                            
                            cJSON_AddBoolToObject(videoObj, "processed", true);
                            cJSON_AddStringToObject(videoObj, "processedUrl", processed_url);
                            
                            // 添加视频FPS信息，暂时使用固定值30.0，可以在视频处理时更新
                            cJSON_AddNumberToObject(videoObj, "fps", 30.0);
                            processedCount++;
                        } else {
                            printf("未处理完成的视频(文件大小为0): %s\n", entry->d_name);
                            cJSON_AddBoolToObject(videoObj, "processed", false);
                        }
                    } else {
                        printf("未处理的视频: %s\n", entry->d_name);
                        cJSON_AddBoolToObject(videoObj, "processed", false);
                    }
                    
                    /* 添加视频对象到数组 */
                    cJSON_AddItemToArray(videoArray, videoObj);
                    videoCount++;
                } else {
                    printf("跳过非视频文件: %s\n", entry->d_name);
                }
            } else {
                printf("跳过非常规文件: %s\n", entry->d_name);
            }
        }
        closedir(dir);
    } else {
        printf("无法打开视频目录: %s\n", VIDEO_DIR);
    }
    
    printf("总共找到 %d 个视频文件，其中 %d 个已处理\n", videoCount, processedCount);
    
    /* 添加视频总数和处理信息 */
    cJSON_AddNumberToObject(datas, "totalCount", videoCount);
    cJSON_AddNumberToObject(datas, "processedCount", processedCount);
    cJSON_AddNumberToObject(datas, "pendingCount", videoCount - processedCount);
    cJSON_AddBoolToObject(datas, "isProcessing", false); // 可以在处理函数中设置正在处理的状态
    
    /* 将JSON对象转换为字符串并写入响应 */
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    /* 清理资源 */
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: getVideoProgress_fun
** 功能描述: 获取当前视频处理进度
** 输　入  : wp
** 输　出  : 无
***************************************************************************************************/
void getVideoProgress_fun(Webs * wp)
{
    cJSON *response = NULL;
    cJSON *datas = NULL;
    cJSON *progressJson = NULL;
    char *jsonString = NULL;
    FILE *fp = NULL;
    char progressBuffer[512];
    size_t bytesRead = 0;
    int current = 0;
    int total = 0;
    double percent = 0.0;
    const char *status = "idle";

    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);

    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();

    fp = fopen(VIDEO_PROGRESS_FILE, "r");
    if (fp) {
        bytesRead = fread(progressBuffer, 1, sizeof(progressBuffer) - 1, fp);
        fclose(fp);
        progressBuffer[bytesRead] = '\0';

        if (bytesRead > 0) {
            progressJson = cJSON_Parse(progressBuffer);
            if (progressJson) {
                cJSON *currentItem = cJSON_GetObjectItem(progressJson, "current");
                cJSON *totalItem = cJSON_GetObjectItem(progressJson, "total");
                cJSON *percentItem = cJSON_GetObjectItem(progressJson, "percent");
                cJSON *statusItem = cJSON_GetObjectItem(progressJson, "status");

                if (cJSON_IsNumber(currentItem)) {
                    current = currentItem->valueint;
                }
                if (cJSON_IsNumber(totalItem)) {
                    total = totalItem->valueint;
                }
                if (cJSON_IsNumber(percentItem)) {
                    percent = percentItem->valuedouble;
                }
                if (cJSON_IsString(statusItem) && statusItem->valuestring) {
                    status = statusItem->valuestring;
                }
            }
        }
    }

    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }

    cJSON_AddNumberToObject(response, "resp_code", 0);
    cJSON_AddStringToObject(response, "resp_msg", "成功获取视频处理进度");
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);

    cJSON_AddNumberToObject(datas, "current", current);
    cJSON_AddNumberToObject(datas, "total", total);
    cJSON_AddNumberToObject(datas, "percent", percent);
    cJSON_AddStringToObject(datas, "status", status);

    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);

    if (progressJson) {
        cJSON_Delete(progressJson);
    }
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: clearAllVideos_fun
** 功能描述: 清理所有视频功能函数
** 输　入  : wp
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void clearAllVideos_fun(Webs * wp)
{
    int originalResult = 0;              /* 原始视频目录清理结果 */
    int processedResult = 0;             /* 处理后视频目录清理结果 */
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    trace(2, "开始清理所有视频文件");
    
    /* 清空原始视频目录 */
    trace(2, "清理原始视频目录: %s", VIDEO_DIR);
    originalResult = remove_dir_contents(VIDEO_DIR, false);
    if (originalResult != 0) {
        trace(1, "清理原始视频目录中的文件失败，错误数: %d", originalResult);
    }
    
    /* 清空处理后视频目录 */
    trace(2, "清理处理后视频目录: %s", PROCESSED_DIR);
    processedResult = remove_dir_contents(PROCESSED_DIR, false);
    if (processedResult != 0) {
        trace(1, "清理处理后视频目录失败，错误数: %d", processedResult);
    }
    
    /* 清理视频列表 */
    trace(2, "清理内存中的视频列表");
    clearVideoList();
    
    /* 创建JSON响应 */
    response = cJSON_CreateObject();
    datas = cJSON_CreateObject();
    
    /* 检查清理结果 */
    if (originalResult == 0 && processedResult == 0) {
        /* 完全成功 */
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "成功清理所有视频");
    } else {
        /* 部分或全部失败 */
        cJSON_AddNumberToObject(response, "resp_code", 1);
        if (originalResult != 0 && processedResult != 0) {
            cJSON_AddStringToObject(response, "resp_msg", "清理视频和处理后视频目录均失败");
        } else if (originalResult != 0) {
            cJSON_AddStringToObject(response, "resp_msg", "清理视频目录失败");
        } else {
            cJSON_AddStringToObject(response, "resp_msg", "清理处理后视频目录失败");
        }
        /* 添加详细错误信息 */
        cJSON_AddNumberToObject(datas, "originalResult", originalResult);
        cJSON_AddNumberToObject(datas, "processedResult", processedResult);
    }
    
    trace(2, "视频清理完成，%s", (originalResult == 0 && processedResult == 0) ? "完全成功" : "有错误发生");
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: setVideoModel_fun
** 功能描述: 设置视频处理模型
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void setVideoModel_fun(Webs * wp)
{
    cchar *modelPath;                       /* 模型路径参数 */
    cJSON *response = NULL;                 /* JSON响应对象 */
    cJSON *datas = NULL;                    /* JSON数据对象 */
    char *jsonString = NULL;                /* JSON字符串 */
    
    websSetStatus(wp, 200);                 /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);            /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);                /* 结束HTTP头部写入 */
    
    /* 获取模型路径参数 */
    modelPath = websGetVar(wp, "modelPath", NULL);
    
    response = cJSON_CreateObject();        /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();           /* 创建数据对象 */
    
    /* 验证模型路径参数 */
    if (modelPath == NULL || *modelPath == '\0') {
        resolveCurrentVideoModel(g_modelPath, sizeof(g_modelPath), true);
        /* 如果没有提供参数，返回当前模型路径 */
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "当前模型路径");
        cJSON_AddStringToObject(datas, "modelPath", g_modelPath);
        printf("当前视频模型: %s\n", g_modelPath);
    } else {
        /* 安全检查 - 防止路径遍历攻击 */
        if (strstr(modelPath, "..") || strchr(modelPath, '/') || strchr(modelPath, '\\')) {
            /* 不安全的模型路径 */
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "不安全的模型路径");
            cJSON_AddStringToObject(datas, "modelPath", g_modelPath);
            printf("视频模型路径安全检查失败: %s\n", modelPath);
        }
        else {
            /* 限制模型路径长度，防止缓冲区溢出 */
            char modelPathCopy[256];
            strncpy(modelPathCopy, modelPath, sizeof(modelPathCopy) - 1);
            modelPathCopy[sizeof(modelPathCopy) - 1] = '\0';
            
            char fullPath[MAX_CMD_LEN];
            build_fs_path(fullPath, sizeof(fullPath), MODEL_DIR, modelPathCopy);
            
            printf("检查视频模型文件: %s\n", fullPath);
            
            /* 检查模型文件是否存在 */
            if (access(fullPath, F_OK) == -1) {
                /* 模型文件不存在 */
                cJSON_AddNumberToObject(response, "resp_code", -1);
                cJSON_AddStringToObject(response, "resp_msg", "模型文件不存在");
                cJSON_AddStringToObject(datas, "modelPath", modelPathCopy);
                printf("视频模型文件不存在: %s\n", modelPathCopy);
            } else {
                /* 模型文件存在，设置新的模型路径 */
                pthread_mutex_lock(&g_mutex);    /* 获取互斥锁 */
                strncpy(g_modelPath, modelPathCopy, sizeof(g_modelPath) - 1);
                g_modelPath[sizeof(g_modelPath) - 1] = '\0';  /* 确保字符串结束 */
                printf("视频模型已切换为: %s\n", g_modelPath);
                pthread_mutex_unlock(&g_mutex);  /* 释放互斥锁 */
                
                cJSON_AddNumberToObject(response, "resp_code", 0);
                cJSON_AddStringToObject(response, "resp_msg", "成功设置模型路径");
                cJSON_AddStringToObject(datas, "modelPath", g_modelPath);
            }
        }
    }
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: getAvailableVideoModels_fun
** 功能描述: 获取可用的视频处理模型列表
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void getAvailableVideoModels_fun(Webs * wp)
{
    DIR *dir;                              /* 目录流指针 */
    struct dirent *entry;                  /* 目录项结构指针 */
    cJSON *response = NULL;                /* JSON响应对象 */
    cJSON *datas = NULL;                   /* JSON数据对象 */
    cJSON *modelList = NULL;               /* 模型列表JSON数组 */
    cJSON *modelObj = NULL;                /* 单个模型JSON对象 */
    char *jsonString = NULL;               /* JSON字符串 */
    bool foundSelected = false;            /* 跟踪是否找到当前选中的模型 */
    
    websSetStatus(wp, 200);                /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);           /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);               /* 结束HTTP头部写入 */
    
    response = cJSON_CreateObject();       /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();          /* 创建数据对象 */
    modelList = cJSON_CreateArray();       /* 创建模型列表数组 */
    
    resolveCurrentVideoModel(g_modelPath, sizeof(g_modelPath), true);

    /* 扫描模型目录 */
    dir = opendir(MODEL_DIR);
    if (!dir) {
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "视频模型目录不可用");
        printf("无法打开视频模型目录\n");
    } else {
        /* 遍历目录下的所有.rknn文件 */
        while ((entry = readdir(dir)) != NULL) {
            /* 忽略.和..目录 */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            /* 只添加.rknn文件 */
            if (isVideoModelFile(entry->d_name)) {

                
                modelObj = cJSON_CreateObject();
                cJSON_AddStringToObject(modelObj, "name", entry->d_name);
                
                /* 检查是否为当前选择的模型 */
                if (strcmp(entry->d_name, g_modelPath) == 0) {
                    cJSON_AddBoolToObject(modelObj, "selected", true);
                    foundSelected = true;

                } else {
                    cJSON_AddBoolToObject(modelObj, "selected", false);
                }
                
                cJSON_AddItemToArray(modelList, modelObj);
            }
        }
        closedir(dir);
        
        /* 检查是否找到了模型文件 */
        if (cJSON_GetArraySize(modelList) == 0) {
            pthread_mutex_lock(&g_mutex);
            g_modelPath[0] = '\0';
            pthread_mutex_unlock(&g_mutex);
        } else if (!foundSelected) {
            /* 如果没有找到当前选择的模型，选择第一个 */
            cJSON *firstModel = cJSON_GetArrayItem(modelList, 0);
            if (firstModel) {
                /* 修改第一个模型的selected属性为true */
                cJSON *selected = cJSON_GetObjectItem(firstModel, "selected");
                if (selected) {
                    cJSON_DeleteItemFromObject(firstModel, "selected");
                }
                cJSON_AddBoolToObject(firstModel, "selected", true);
                
                /* 更新当前模型路径 */
                cJSON *nameObj = cJSON_GetObjectItem(firstModel, "name");
                if (nameObj && cJSON_IsString(nameObj)) {
                    strncpy(g_modelPath, cJSON_GetStringValue(nameObj), sizeof(g_modelPath) - 1);
                    g_modelPath[sizeof(g_modelPath) - 1] = '\0';

                }
            }
        }
        
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "成功获取视频模型列表");

    }
    
    /* 添加当前选择的模型 */
    cJSON_AddStringToObject(datas, "currentModel", g_modelPath);
    cJSON_AddItemToObject(datas, "models", modelList);
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    // 简化日志输出，只显示关键信息
    printf("视频模型列表已返回，当前选中模型: %s\n", g_modelPath);
    
    // 使用sync()函数替代system("sync")
    sync();
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/**
 * 初始化视频处理系统
 * 扫描现有视频、创建必要目录
 * @return 0表示成功，非0表示部分失败
 */
int initVideoProcessingSystem(void)
{
    DIR             *dir;
    struct dirent   *entry;
    char            filename[256];
    char            videoFilePath[512];
    char            *documentRoot;
    int             count = 0;
    int             errors = 0;
    struct stat     st;
    char            video_dir[MAX_PATH_LENGTH];    /* 视频目录路径 */
    char            processed_dir[MAX_PATH_LENGTH]; /* 处理后视频目录路径 */
    char            model_dir[MAX_PATH_LENGTH];     /* 模型目录路径 */
    
    logmsg(2, "开始初始化视频处理系统");
    
    // 获取文档根目录
    documentRoot = websGetDocuments();
    if (!documentRoot) {
        logmsg(1, "无法获取文档根目录");
        return -1;
    }
    
    resolveCurrentVideoModel(g_modelPath, sizeof(g_modelPath), true);
    
    // 初始化目录结构
    initVideoDirs();
    
    // 构建目录路径
    build_fs_path(video_dir, sizeof(video_dir), VIDEO_DIR, NULL);
    build_fs_path(processed_dir, sizeof(processed_dir), PROCESSED_DIR, NULL);
    build_fs_path(model_dir, sizeof(model_dir), MODEL_DIR, NULL);
    
    // 确保目录具有适当的权限
    if (ensureVideoDirectoryPermissions(video_dir, 0755) != 0) {
        logmsg(1, "警告: 无法确保视频目录权限: %s", video_dir);
        errors++;
    } else {
        logmsg(2, "已确保视频目录具有读写权限: %s", video_dir);
    }
    
    if (ensureVideoDirectoryPermissions(processed_dir, 0755) != 0) {
        logmsg(1, "警告: 无法确保处理后视频目录权限: %s", processed_dir);
        errors++;
    } else {
        logmsg(2, "已确保处理后视频目录具有读写权限: %s", processed_dir);
    }
    
    if (ensureVideoDirectoryPermissions(model_dir, 0755) != 0) {
        logmsg(1, "警告: 无法确保模型目录权限: %s", model_dir);
        errors++;
    } else {
        logmsg(2, "已确保模型目录具有读写权限: %s", model_dir);
    }
    
    // 清理旧的视频列表
    clearVideoList();
    
    // 扫描已有视频文件 - 修复路径构建，直接使用VIDEO_DIR而不是与documentRoot连接
    strncpy(videoFilePath, VIDEO_DIR, sizeof(videoFilePath) - 1);
    videoFilePath[sizeof(videoFilePath) - 1] = '\0';
    
    dir = opendir(videoFilePath);
    if (!dir) {
        logmsg(1, "无法打开视频目录: %s", videoFilePath);
        return -1;
    }
    
    // 遍历目录中的所有文件
    while ((entry = readdir(dir)) != NULL) {
        // 跳过隐藏文件和目录
        if (entry->d_name[0] == '.') {
            continue;
        }
        
        build_fs_path(filename, sizeof(filename), videoFilePath, entry->d_name);
        
        // 检查文件类型
        if (stat(filename, &st) == 0 && S_ISREG(st.st_mode)) {
            // 向列表中添加视频文件
            VideoInfo *video = (VideoInfo*)malloc(sizeof(VideoInfo));
            if (video) {
                strcpy(video->filename, filename);
                strcpy(video->clientFilename, entry->d_name);
                
                // 检查是否有对应的处理后文件
                char processedPath[512];
                
                // 修复处理后文件的路径构建
                build_fs_path(processedPath, sizeof(processedPath), PROCESSED_DIR, entry->d_name);
                
                video->processed = (access(processedPath, F_OK) == 0);
                // 添加到全局视频列表
                if (g_videoCount < MAX_VIDEOS) {
                    // 记录状态以便日志记录
                    bool processed = video->processed;
                    char clientName[256];
                    strncpy(clientName, entry->d_name, sizeof(clientName)-1);
                    clientName[sizeof(clientName)-1] = '\0';
                    
                    // 复制结构体的内容而不是赋值指针
                    memcpy(&g_videos[g_videoCount], video, sizeof(VideoInfo));
                    g_videoCount++;
                    count++;
                    
                    // 释放临时分配的内存
                    free(video);
                    
                    logmsg(3, "已加载视频: %s (处理状态: %s)", 
                          clientName, processed ? "已处理" : "未处理");
                }
                
                if (g_videoCount >= MAX_VIDEOS) {
                    logmsg(1, "警告: 视频数量达到最大限制 %d", MAX_VIDEOS);
                    break;
                }
            } else {
                logmsg(1, "警告: 无法分配内存给视频信息结构体");
                errors++;
            }
        }
    }
    
    closedir(dir);
    
    logmsg(2, "视频处理系统初始化完成，共加载 %d 个视频文件", count);
    
    return (errors > 0) ? errors : 0;
}

/***************************************************************************************************
** 函数名称: processVideoFile
** 功能描述: 处理单个视频文件
** 输　入  : filename - 视频文件名
** 输　出  : 处理结果，成功返回true，失败返回false
** 全局变量:
** 调用模块:
***************************************************************************************************/
static bool processVideoFile(const char* filename)
{
    char srcPath[MAX_PATH_LENGTH];                /* 源视频路径 */
    char destPath[MAX_PATH_LENGTH];               /* 处理后视频保存路径 */
    char currentModel[256];                       /* 当前使用的模型名称 */
    struct stat outputStat;
    int result = -1;                              /* 处理结果 */
    
    if (filename == NULL) {
        error("视频文件名为空");
        return false;
    }
    
    printf("开始处理视频: %s\n", filename);
    
    /* 构建源文件和目标文件路径 */
    build_fs_path(srcPath, sizeof(srcPath), VIDEO_DIR, filename);  /* 原始视频完整路径 */
    build_fs_path(destPath, sizeof(destPath), PROCESSED_DIR, filename); /* 处理后视频完整路径 */
    
    /* 检查视频文件是否存在 */
    if (access(srcPath, F_OK) == -1) {
        error("视频文件不存在: %s", srcPath);
        return false;
    }
    
    resolveCurrentVideoModel(currentModel, sizeof(currentModel), true);
    
    printf("视频处理使用模型: %s\n", currentModel);

    /* 清理旧进度，避免前端读到上一次任务的残留状态 */
    unlink(VIDEO_PROGRESS_FILE);
    
    /* 使用fork/exec替代system调用 */
    pid_t child_pid = fork();
    
    if (child_pid == -1) {
        /* fork失败 */
        error("创建子进程失败");
        return false;
    } else if (child_pid == 0) {
        /* 子进程 */
        
        /* 切换到模型目录 */
        if (chdir(MODEL_DIR) != 0) {
            fprintf(stderr, "无法切换到模型目录: %s\n", MODEL_DIR);
            exit(1);
        }
        
        /* 构建相对路径参数 - 调整相对路径计算，从web/model/video访问web/videos只需上升一级 */
        char relSrcPath[MAX_PATH_LENGTH], relDestPath[MAX_PATH_LENGTH];
        snprintf(relSrcPath, sizeof(relSrcPath), "../../../%s", srcPath);
        snprintf(relDestPath, sizeof(relDestPath), "../../../%s", destPath);
        
        /* 使用execlp从PATH中查找python3，并传递相对路径参数 */
        execlp("python3", "python3", "test.py",
               "--input", relSrcPath, 
               "--output", relDestPath, 
               "--model", currentModel, NULL);
        
        /* 如果execlp失败，会执行到这里 */
        fprintf(stderr, "执行python脚本失败: %s\n", strerror(errno));
        exit(1);
    } else {
        /* 父进程 */
        int status;
        waitpid(child_pid, &status, 0);
        
        if (WIFEXITED(status)) {
            result = WEXITSTATUS(status);
            printf("视频处理完成，结果代码: %d\n", result);
        } else {
            error("视频处理异常终止");
            result = -1;
        }
    }
    
    if (result == 0) {
        /* 处理成功，检查输出文件 */
        if (stat(destPath, &outputStat) == 0) {
            if (outputStat.st_size > 0) {
                printf("视频处理成功: %s\n", filename);
                return true;
            }
        }
        printf("命令执行成功但未生成有效输出文件: %s\n", destPath);
        return false;
    } else {
        printf("视频处理命令执行失败，返回码: %d\n", result);
        return false;
    }
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
** 函数名称: build_url_path
** 功能描述: 构建URL路径，统一URL处理方式
** 输　入  : buffer - 输出缓冲区, buffer_size - 缓冲区大小, prefix - URL前缀, filename - 文件名
** 输　出  : 返回buffer指针，方便链式调用
***************************************************************************************************/
static char* build_url_path(char* buffer, size_t buffer_size, const char* prefix, const char* filename) {
    if (!buffer || buffer_size == 0 || !prefix) {
        return NULL;
    }
    
    if (filename) {
        snprintf(buffer, buffer_size, "%s/%s", prefix, filename);
    } else {
        snprintf(buffer, buffer_size, "%s", prefix);
    }
    
    // 确保字符串以null结尾
    buffer[buffer_size - 1] = '\0';
    
    return buffer;
}

/***************************************************************************************************
** 函数名称: ensureVideoDirectoryPermissions
** 功能描述: 确保视频目录具有适当的读写权限
** 输　入  : dirPath - 需要设置权限的目录路径, mode - 要设置的权限模式
** 输　出  : 成功返回0，失败返回非0值
***************************************************************************************************/
static int ensureVideoDirectoryPermissions(const char *dirPath, mode_t mode) {
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
