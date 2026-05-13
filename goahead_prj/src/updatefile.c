/********************************* Includes ***********************************/

#include    "goahead.h"       /* 包含GoAhead Web服务器的头文件 */
#include    "updatefile.h"    /* 包含当前模块的头文件声明 */
#include    "cJSON.h"         /* 包含JSON解析和生成库 */
#include    <pthread.h>       /* 包含POSIX线程库，用于多线程处理 */
#include    <stdbool.h>       /* 包含布尔类型定义 */
#include    <dirent.h>        /* 包含目录操作相关的函数和结构体 */
#include    <sys/stat.h>      /* 包含文件状态查询相关的函数和结构体 */
#include    <unistd.h>        /* 包含POSIX标准函数如getcwd等 */
#include    <errno.h>         /* 包含errno.h头文件 */
#ifdef _WIN32
#include    <process.h>      /* Windows进程控制 */
#else
#include    <sys/wait.h>      /* POSIX waitpid函数 */
#endif

/********************************* Defines ************************************/

#define MAX_IMAGES 100                  /* 定义最大处理图片数量 */
#define IMAGE_DIR "web/images"        /* 定义上传图片存储目录 */
#define PROCESSED_DIR "web/processed" /* 定义处理后图片存储目录 */
#define IMAGE_URL_PREFIX "/images"      /* 定义原始图片URL前缀 */
#define PROCESSED_URL_PREFIX "/processed" /* 定义处理后图片URL前缀 */
#define MODEL_DIR "web/model/image"       /* 定义模型存储目录 */
#define DEFAULT_MODEL ""                /* 默认模型由运行时自动解析 */
#define WORKER_THREADS 4             /* 定义工作线程数 */
#define STATE_FILE "web/image_states.json"  /* 定义状态文件路径，用于永久化存储图片状态 */

/********************************* Globals ************************************/

typedef struct {                       /* 定义图片信息结构体 */
    char filename[256];                /* 完整文件路径 */
    char clientFilename[256];          /* 客户端原始文件名 */
    bool processed;                    /* 处理状态标志 */
} ImageInfo;

static ImageInfo g_images[MAX_IMAGES]; /* 图片信息数组，存储所有待处理和已处理的图片 */
static int g_imageCount = 0;           /* 当前图片总数 */
static bool g_isProcessing = false;    /* 处理状态标志，表示是否正在进行处理 */
static int g_currentImageIndex = 0;    /* 当前正在处理的图片索引 */
static pthread_t g_processThread;      /* 处理线程ID */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER; /* 互斥锁，保护共享数据 */
static char g_modelPath[256] = DEFAULT_MODEL; /* 当前使用的模型路径，默认为best.rknn */
static int g_processedCount = 0;       /* 已处理图片计数 */
static int g_failedCount = 0;          /* 处理失败计数 */
static int g_skipCount = 0;            /* 跳过图片计数 */

/********************************* Forwards ***********************************/

static void* processImagesThread(void* arg); /* 图片批量处理线程函数声明 */
static void initImageDirs(void);             /* 初始化图片存储目录函数声明 */
static void clearImageList(void);            /* 清空图片列表函数声明 */
static int copyFile(const char* source, const char* destination); /* 高效的文件复制函数声明 */
static int mkdirRecursive(const char *path, mode_t mode); /* 递归创建目录函数声明 */
static int clearDirectory(const char *dirPath); /* 清空目录中所有文件函数声明 */
static int ensureDirectoryPermissions(const char *dirPath, mode_t mode); /* 确保目录具有适当权限函数声明 */
static bool isSupportedImageFile(const char *filename);
static bool isModelFile(const char *filename);
static bool resolveCurrentModel(char *buffer, size_t bufferSize, bool updateGlobal);
static void removeProcessedImageFor(const char *filename);
static void upsertImageRecord(const char *clientFilename, bool processed);

/* 优化的文件复制函数 - 专为大量小图片场景优化 */
static int copyFile(const char* source, const char* destination) {
    FILE *src, *dst;
    char buffer[65536]; // 增加到64KB缓冲区，提高I/O效率
    size_t bytesRead;
    
    src = fopen(source, "rb");
    if (src == NULL) return 0;
    
    dst = fopen(destination, "wb");
    if (dst == NULL) {
        fclose(src);
        return 0;
    }
    
    // 使用setvbuf优化I/O缓冲
    setvbuf(src, NULL, _IOFBF, 65536);
    setvbuf(dst, NULL, _IOFBF, 65536);
    
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytesRead, dst) != bytesRead) {
            fclose(src);
            fclose(dst);
            return 0;
        }
    }
    
    fclose(src);
    fclose(dst);
    return 1;
}

/* 递归创建目录，类似于mkdir -p */
static int mkdirRecursive(const char *path, mode_t mode)
{
    char tmp[256];
    char *p = NULL;
    size_t len;
    int ret;
    
    /* 复制路径，避免修改原始字符串 */
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    /* 确保路径末尾没有'/' */
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    /* 遍历路径，逐级创建目录 */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            ret = mkdir(tmp, mode);
            if (ret != 0 && errno != EEXIST) {
                error("创建目录失败: %s, 错误: %s", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    
    /* 创建最终目录 */
    ret = mkdir(tmp, mode);
    if (ret != 0 && errno != EEXIST) {
        error("创建目录失败: %s, 错误: %s", tmp, strerror(errno));
        return -1;
    }
    
    return 0;
}

/***************************************************************************************************
** 函数名称: initImageDirs
** 功能描述: 初始化图片存储目录
** 输　入  : 无
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
static void initImageDirs(void)
{
    /* 使用递归方式创建目录，确保多级目录结构都能创建成功 */
    
    /* 创建原始图片目录 */
    if (mkdirRecursive(IMAGE_DIR, 0755) != 0) {
        error("无法创建原始图片目录: %s", IMAGE_DIR);
    } else {
        trace(2, "成功创建或验证目录: %s", IMAGE_DIR);
    }
    
    /* 创建处理后图片目录 */
    if (mkdirRecursive(PROCESSED_DIR, 0755) != 0) {
        error("无法创建处理后图片目录: %s", PROCESSED_DIR);
    } else {
        trace(2, "成功创建或验证目录: %s", PROCESSED_DIR);
    }
}

/***************************************************************************************************
** 函数名称: clearImageList
** 功能描述: 清空图片列表
** 输　入  : 无
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
static void clearImageList(void)
{
    pthread_mutex_lock(&g_mutex);            /* 获取互斥锁，保护共享数据 */
    g_imageCount = 0;                        /* 重置图片计数为0 */
    g_currentImageIndex = 0;                 /* 重置当前处理图片索引为0 */
    memset(g_images, 0, sizeof(g_images));   /* 清空图片信息数组 */
    pthread_mutex_unlock(&g_mutex);          /* 释放互斥锁 */
}

static bool isSupportedImageFile(const char *filename)
{
    char *ext;

    if (!filename || !*filename) {
        return false;
    }
    ext = strrchr(filename, '.');
    return ext && (scaselesscmp(ext, ".jpg") == 0 ||
        scaselesscmp(ext, ".jpeg") == 0 ||
        scaselesscmp(ext, ".png") == 0 ||
        scaselesscmp(ext, ".bmp") == 0);
}

static bool isModelFile(const char *filename)
{
    char *ext;

    if (!filename || !*filename) {
        return false;
    }
    ext = strrchr(filename, '.');
    return ext && scaselesscmp(ext, ".rknn") == 0;
}

static bool resolveCurrentModel(char *buffer, size_t bufferSize, bool updateGlobal)
{
    DIR             *dir;
    struct dirent   *entry;
    char            firstModel[256];
    char            selectedModel[256];
    bool            foundCurrent;

    if (!buffer || bufferSize == 0) {
        return false;
    }
    buffer[0] = '\0';
    firstModel[0] = '\0';
    selectedModel[0] = '\0';
    foundCurrent = false;

    dir = opendir(MODEL_DIR);
    if (!dir) {
        logmsg(1, "无法打开图片模型目录: %s", MODEL_DIR);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!isModelFile(entry->d_name)) {
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

    strncpy(buffer, selectedModel, bufferSize - 1);
    buffer[bufferSize - 1] = '\0';

    if (updateGlobal) {
        pthread_mutex_lock(&g_mutex);
        strncpy(g_modelPath, selectedModel, sizeof(g_modelPath) - 1);
        g_modelPath[sizeof(g_modelPath) - 1] = '\0';
        pthread_mutex_unlock(&g_mutex);
    }
    return true;
}

static void removeProcessedImageFor(const char *filename)
{
    char processedPath[512];

    if (!filename || !*filename) {
        return;
    }
    snprintf(processedPath, sizeof(processedPath), "%s/%s", PROCESSED_DIR, filename);
    if (access(processedPath, F_OK) == 0 && unlink(processedPath) != 0) {
        logmsg(1, "删除旧处理结果失败: %s, 错误: %s", processedPath, strerror(errno));
    }
}

static void upsertImageRecord(const char *clientFilename, bool processed)
{
    int i;

    if (!clientFilename || !*clientFilename) {
        return;
    }

    pthread_mutex_lock(&g_mutex);
    for (i = 0; i < g_imageCount; i++) {
        if (strcmp(g_images[i].clientFilename, clientFilename) == 0) {
            snprintf(g_images[i].filename, sizeof(g_images[i].filename), "%s/%s", IMAGE_DIR, clientFilename);
            g_images[i].processed = processed;
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }
    if (g_imageCount < MAX_IMAGES) {
        snprintf(g_images[g_imageCount].filename, sizeof(g_images[g_imageCount].filename), "%s/%s", IMAGE_DIR, clientFilename);
        strncpy(g_images[g_imageCount].clientFilename, clientFilename, sizeof(g_images[g_imageCount].clientFilename) - 1);
        g_images[g_imageCount].clientFilename[sizeof(g_images[g_imageCount].clientFilename) - 1] = '\0';
        g_images[g_imageCount].processed = processed;
        g_imageCount++;
    }
    pthread_mutex_unlock(&g_mutex);
}

/***************************************************************************************************
** 函数名称: saveImageStates
** 功能描述: 保存图片状态到文件
** 输　入  : 无
** 输　出  : 无
** 全局变量: g_imageCount, g_images
** 调用模块:
***************************************************************************************************/
static bool saveImageStates(void)
{
    FILE *fp;                            /* 文件指针 */
    cJSON *root = NULL;                  /* JSON根对象 */
    cJSON *imageArray = NULL;            /* 图片数组 */
    cJSON *imageObj = NULL;              /* 单个图片对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    int i;                               /* 循环索引 */
    
    /* 创建JSON对象 */
    root = cJSON_CreateObject();
    if (!root) {
        error("创建JSON对象失败");
        return false;
    }
    
    /* 创建图片数组 */
    imageArray = cJSON_CreateArray();
    if (!imageArray) {
        cJSON_Delete(root);
        error("创建JSON数组失败");
        return false;
    }
    
    /* 添加版本信息 */
    cJSON_AddStringToObject(root, "version", "1.0");
    
    /* 添加所有图片信息 */
    pthread_mutex_lock(&g_mutex);        /* 获取互斥锁，保护共享数据 */
    for (i = 0; i < g_imageCount; i++) {
        imageObj = cJSON_CreateObject();
        if (!imageObj) {
            continue;
        }
        
        cJSON_AddStringToObject(imageObj, "filename", g_images[i].filename);
        cJSON_AddStringToObject(imageObj, "clientFilename", g_images[i].clientFilename);
        cJSON_AddBoolToObject(imageObj, "processed", g_images[i].processed);
        
        cJSON_AddItemToArray(imageArray, imageObj);
    }
    pthread_mutex_unlock(&g_mutex);      /* 释放互斥锁 */
    
    /* 添加图片数组到根对象 */
    cJSON_AddItemToObject(root, "images", imageArray);
    
    /* 转换为字符串 */
    jsonString = cJSON_Print(root);
    if (!jsonString) {
        cJSON_Delete(root);
        error("转换JSON到字符串失败");
        return false;
    }
    
    /* 写入文件 */
    fp = fopen(STATE_FILE, "w");
    if (!fp) {
        free(jsonString);
        cJSON_Delete(root);
        error("无法打开状态文件 %s 进行写入", STATE_FILE);
        return false;
    }
    
    fprintf(fp, "%s", jsonString);
    fclose(fp);
    
    /* 清理资源 */
    free(jsonString);
    cJSON_Delete(root);
    
    trace(2, "已保存 %d 张图片的状态信息到 %s", g_imageCount, STATE_FILE);
    return true;
}

/***************************************************************************************************
** 函数名称: loadImageStates
** 功能描述: 从文件加载图片状态并与文件系统同步
** 输　入  : 无
** 输　出  : 成功返回true，失败返回false
** 全局变量: g_imageCount, g_images
** 调用模块:
***************************************************************************************************/
static bool loadImageStates(void)
{
    FILE *fp;                            /* 文件指针 */
    char *buffer = NULL;                 /* 文件缓冲区 */
    long fileSize;                       /* 文件大小 */
    cJSON *root = NULL;                  /* JSON根对象 */
    cJSON *imageArray = NULL;            /* 图片数组 */
    cJSON *imageObj = NULL;              /* 单个图片对象 */
    int i, j;                            /* 循环索引 */
    bool success = false;                /* 成功标志 */
    char imagePath[512];                 /* 图片路径 */
    DIR *dir;                            /* 目录指针 */
    struct dirent *entry;                /* 目录项 */
    struct stat st;                      /* 文件状态 */
    bool imageFound = false;             /* 图片是否找到 */
    char processedPath[512];             /* 处理后图片路径 */
    bool statusChanged = false;          /* 状态是否改变 */
    ImageInfo tempImages[MAX_IMAGES];    /* 临时图片信息数组 */
    int tempImageCount = 0;              /* 临时图片计数 */
    
    /* 先扫描图片目录，获取实际存在的图片 */
    dir = opendir(IMAGE_DIR);
    if (!dir) {
        error("无法打开图片目录 %s", IMAGE_DIR);
        return false;
    }
    
    /* 扫描所有图片 */
    while ((entry = readdir(dir)) != NULL && tempImageCount < MAX_IMAGES) {
        /* 忽略.和..目录 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* 构建完整路径 */
        sprintf(imagePath, "%s/%s", IMAGE_DIR, entry->d_name);
        
        /* 检查是否为常规文件 */
        if (stat(imagePath, &st) == 0 && S_ISREG(st.st_mode)) {
            /* 只处理常见图片格式 */
            char *ext = strrchr(entry->d_name, '.');
            if (ext && (scaselesscmp(ext, ".jpg") == 0 || scaselesscmp(ext, ".jpeg") == 0 || 
                       scaselesscmp(ext, ".png") == 0 || scaselesscmp(ext, ".bmp") == 0)) {
                
                /* 构建处理后图片路径 */
                sprintf(processedPath, "%s/%s", PROCESSED_DIR, entry->d_name);
                
                /* 记录图片信息 */
                strncpy(tempImages[tempImageCount].filename, imagePath, sizeof(tempImages[tempImageCount].filename) - 1);
                strncpy(tempImages[tempImageCount].clientFilename, entry->d_name, sizeof(tempImages[tempImageCount].clientFilename) - 1);
                
                /* 检查是否已处理 - 文件存在且不为空 */
                if (access(processedPath, F_OK) != -1) {
                    struct stat proc_st;
                    if (stat(processedPath, &proc_st) == 0 && proc_st.st_size > 0) {
                        tempImages[tempImageCount].processed = true;
                    } else {
                        tempImages[tempImageCount].processed = false;
                    }
                } else {
                    tempImages[tempImageCount].processed = false;
                }
                
                tempImageCount++;
            }
        }
    }
    closedir(dir);
    
    /* 打开状态文件 */
    fp = fopen(STATE_FILE, "r");
    if (!fp) {
        trace(2, "状态文件 %s 不存在，将使用扫描结果创建新的状态文件", STATE_FILE);
        
        /* 直接使用扫描结果 */
        pthread_mutex_lock(&g_mutex);
        g_imageCount = tempImageCount;
        for (i = 0; i < tempImageCount; i++) {
            g_images[i] = tempImages[i];
        }
        pthread_mutex_unlock(&g_mutex);
        
        /* 保存状态到文件 */
        saveImageStates();
        return true;
    }
    
    /* 获取文件大小 */
    fseek(fp, 0, SEEK_END);
    fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (fileSize <= 0) {
        fclose(fp);
        trace(2, "状态文件为空，将使用扫描结果");
        
        /* 直接使用扫描结果 */
        pthread_mutex_lock(&g_mutex);
        g_imageCount = tempImageCount;
        for (i = 0; i < tempImageCount; i++) {
            g_images[i] = tempImages[i];
        }
        pthread_mutex_unlock(&g_mutex);
        
        /* 保存状态到文件 */
        saveImageStates();
        return true;
    }
    
    /* 分配内存并读取文件 */
    buffer = (char*)malloc(fileSize + 1);
    if (!buffer) {
        fclose(fp);
        error("内存分配失败");
        return false;
    }
    
    size_t readSize = fread(buffer, 1, fileSize, fp);
    fclose(fp);
    
    if (readSize != (size_t)fileSize) {
        free(buffer);
        error("读取状态文件失败");
        return false;
    }
    
    buffer[fileSize] = '\0';
    
    /* 解析JSON */
    root = cJSON_Parse(buffer);
    free(buffer);
    
    if (!root) {
        error("解析状态文件JSON失败");
        
        /* 直接使用扫描结果 */
        pthread_mutex_lock(&g_mutex);
        g_imageCount = tempImageCount;
        for (i = 0; i < tempImageCount; i++) {
            g_images[i] = tempImages[i];
        }
        pthread_mutex_unlock(&g_mutex);
        
        /* 保存状态到文件 */
        saveImageStates();
        return true;
    }
    
    /* 获取图片数组 */
    imageArray = cJSON_GetObjectItem(root, "images");
    if (!imageArray || !cJSON_IsArray(imageArray)) {
        cJSON_Delete(root);
        error("状态文件缺少images数组");
        
        /* 直接使用扫描结果 */
        pthread_mutex_lock(&g_mutex);
        g_imageCount = tempImageCount;
        for (i = 0; i < tempImageCount; i++) {
            g_images[i] = tempImages[i];
        }
        pthread_mutex_unlock(&g_mutex);
        
        /* 保存状态到文件 */
        saveImageStates();
        return true;
    }
    
    /* 遍历JSON中的图片，与扫描结果合并 */
    pthread_mutex_lock(&g_mutex);
    g_imageCount = 0;
    
    /* 首先处理扫描到的每个图片 */
    for (i = 0; i < tempImageCount; i++) {
        imageFound = false;
        
        /* 在状态文件中寻找该图片 */
        int jsonImageCount = cJSON_GetArraySize(imageArray);
        for (j = 0; j < jsonImageCount; j++) {
            imageObj = cJSON_GetArrayItem(imageArray, j);
            if (!imageObj) continue;
            
            cJSON *clientFilename = cJSON_GetObjectItem(imageObj, "clientFilename");
            if (!clientFilename) continue;
            
            /* 检查文件名是否匹配 */
            if (strcmp(cJSON_GetStringValue(clientFilename), tempImages[i].clientFilename) == 0) {
                imageFound = true;
                
                /* 使用状态文件中的处理状态，但要验证处理后文件是否真实存在 */
                cJSON *processed = cJSON_GetObjectItem(imageObj, "processed");
                bool stateProcessed = processed ? cJSON_IsTrue(processed) : false;
                
                /* 根据状态文件和实际文件状态确定处理状态 */
                if (stateProcessed != tempImages[i].processed) {
                    statusChanged = true;
                    /* 以实际文件状态为准 */
                }
                
                break;
            }
        }
        
        /* 添加到全局图片列表 */
        g_images[g_imageCount] = tempImages[i];
        g_imageCount++;
    }
    
    pthread_mutex_unlock(&g_mutex);
    
    /* 清理资源 */
    cJSON_Delete(root);
    
    /* 如果状态有变化，更新状态文件 */
    if (statusChanged) {
        saveImageStates();
    }
    
    success = true;
    trace(2, "成功加载并验证了 %d 张图片的状态", g_imageCount);
    
    return success;
}

/***************************************************************************************************
** 函数名称: processImage
** 功能描述: 处理单张图片
** 输　入  : index - 图片索引
** 输　出  : 处理结果，成功返回true，失败返回false
** 全局变量:
** 调用模块:
***************************************************************************************************/
static bool processImage(int index)
{
    char srcPath[256];                      /* 源图片路径 */
    char destPath[256];                     /* 处理后图片保存路径 */
    char modelPath[512];                    /* 完整模型路径 */
    static char workDir[256] = {0};         /* 当前工作目录（静态变量，只获取一次） */
    static char scriptPath[512] = {0};      /* Python脚本路径（静态变量） */
    char selectedModel[256];
    char clientFilename[256];
    int result = -1;
    bool outputReady = false;
    struct stat processedStat;

    /* 检查索引是否有效 */
    if (index < 0 || index >= g_imageCount) {
        error("无效的图片索引: %d", index);
        return false;                       /* 索引无效，返回失败 */
    }

    pthread_mutex_lock(&g_mutex);           /* 获取互斥锁，保护共享数据 */
    /* 如果已经停止处理，直接返回 */
    if (!g_isProcessing) {
        pthread_mutex_unlock(&g_mutex);     /* 释放互斥锁 */
        return false;                       /* 返回失败 */
    }

    /* 安全检查：确保文件名不包含危险字符 */
    strncpy(clientFilename, g_images[index].clientFilename, sizeof(clientFilename) - 1);
    clientFilename[sizeof(clientFilename) - 1] = '\0';
    if (strstr(clientFilename, "..") != NULL || strstr(clientFilename, "/") != NULL || 
        strstr(clientFilename, "\\") != NULL || strstr(clientFilename, "|") != NULL ||
        strstr(clientFilename, ";") != NULL) {
        error("检测到非法文件名: %s", clientFilename);
        pthread_mutex_unlock(&g_mutex);     /* 释放互斥锁 */
        return false;
    }

    /* 构建源文件和目标文件路径 - 使用安全的字符串函数 */
    snprintf(srcPath, sizeof(srcPath), "%s/%s", IMAGE_DIR, clientFilename);  /* 原始图片完整路径 */
    snprintf(destPath, sizeof(destPath), "%s/%s", PROCESSED_DIR, clientFilename); /* 处理后图片完整路径 */
    
    /* 获取当前处理的图片索引 */
    g_currentImageIndex = index;            /* 更新当前处理图片索引 */
    pthread_mutex_unlock(&g_mutex);         /* 释放互斥锁 */

    resolveCurrentModel(selectedModel, sizeof(selectedModel), true);
    if (selectedModel[0] != '\0') {
        snprintf(modelPath, sizeof(modelPath), "%s/%s", MODEL_DIR, selectedModel);
    } else {
        modelPath[0] = '\0';
    }

    /* 获取当前工作目录（仅首次执行） */
    if (workDir[0] == '\0') {
        if (getcwd(workDir, sizeof(workDir)) == NULL) {
            error("获取当前工作目录失败");
            return false;
        }
        /* 预先构建脚本路径 */
        snprintf(scriptPath, sizeof(scriptPath), "%s/%s/yolov5_test.py", workDir, MODEL_DIR);
    }

    /* 构建完整的绝对路径 - 只针对变化的部分，使用安全函数 */
    char fullSrcPath[512];
    char fullDestPath[512];
    char fullModelPath[512];
    
    snprintf(fullSrcPath, sizeof(fullSrcPath), "%s/%s", workDir, srcPath);
    snprintf(fullDestPath, sizeof(fullDestPath), "%s/%s", workDir, destPath);
    if (modelPath[0] != '\0') {
        snprintf(fullModelPath, sizeof(fullModelPath), "%s/%s", workDir, modelPath);
    } else {
        fullModelPath[0] = '\0';
    }

    /* 检查文件是否存在 */
    if (access(fullSrcPath, F_OK) == -1) {
        error("源文件不存在: %s", fullSrcPath);
        return false;
    }

    /* 使用更安全的方式运行Python脚本处理图片 */
    printf("处理图片: %s -> %s (模型: %s)\n", fullSrcPath, fullDestPath,
        fullModelPath[0] != '\0' ? fullModelPath : "<fallback>");
    
    /* 提供系统兼容性 */
#ifdef _WIN32
    /* Windows实现 - 使用_spawnlp而不是system，避免命令注入风险 */
    char oldWorkDir[256] = {0};
    
    /* 保存当前工作目录 */
    if (getcwd(oldWorkDir, sizeof(oldWorkDir)) == NULL) {
        error("获取当前工作目录失败");
        return false;
    }
    
    /* 切换到模型目录 */
    if (_chdir(MODEL_DIR) != 0) {
        error("无法切换到模型目录: %s", MODEL_DIR);
        return false;
    }
    
    /* 直接调用Python解释器，参数作为独立参数传递，防止命令注入 */
    result = _spawnlp(_P_WAIT, "python", "python", "yolov5_test.py", 
             fullSrcPath, fullDestPath, fullModelPath, NULL);
    
    /* 恢复工作目录 */
    if (_chdir(oldWorkDir) != 0) {
        error("无法恢复工作目录");
    }
    
    printf("图片处理完成，结果: %d\n", result);
#else
    /* Linux/Unix实现 - 使用fork/exec */
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
        
        /* 使用execlp从PATH中查找python3，更加灵活 */
        execlp("python3", "python3", "yolov5_test.py",
               fullSrcPath, fullDestPath, fullModelPath[0] != '\0' ? fullModelPath : "", NULL);
        
        /* 如果execl失败，会继续执行这里的代码 */
        fprintf(stderr, "执行python脚本失败: %s\n", strerror(errno));
        exit(1);
    } else {
        /* 父进程 */
        int status;
        waitpid(child_pid, &status, 0);

        if (WIFEXITED(status)) {
            result = WEXITSTATUS(status);
            printf("图片处理完成，结果: %d\n", result);
        } else {
            error("图片处理异常终止");
            result = -1;
        }
    }
#endif  /* 结束 #ifdef _WIN32 条件块 */

    outputReady = (result == 0 && stat(fullDestPath, &processedStat) == 0 && processedStat.st_size > 0);

    /* 处理完成后更新状态 */
    pthread_mutex_lock(&g_mutex);           /* 再次获取互斥锁 */
    g_images[index].processed = outputReady;
    pthread_mutex_unlock(&g_mutex);         /* 释放互斥锁 */

    saveImageStates();

    return outputReady;                     /* 根据处理结果返回成功或失败 */
}

/***************************************************************************************************
** 函数名称: processImagesThread
** 功能描述: 批量处理图片的线程函数
** 输　入  : arg - 传递给线程的参数
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
static void* processImagesThread(void* arg)
{
    int i;                                /* 循环索引 */
    bool success;                         /* 处理结果标志 */
    int processedCount = 0;               /* 已处理图片计数 */
    int skipCount = 0;                    /* 跳过图片计数 */
    int failedCount = 0;                  /* 处理失败计数 */

    /* 遍历所有图片 */
    for (i = 0; i < g_imageCount; i++) {
        pthread_mutex_lock(&g_mutex);     /* 获取互斥锁 */
        
        /* 检查是否需要停止处理 */
        if (!g_isProcessing) {            /* 如果处理已停止 */
            pthread_mutex_unlock(&g_mutex); /* 释放互斥锁 */
            break;                         /* 跳出循环 */
        }
        
        /* 检查图片是否已处理 */
        if (g_images[i].processed) {      /* 如果图片已处理 */
            g_currentImageIndex = i;      /* 更新当前索引(为了进度显示) */
            skipCount++;                  /* 增加跳过计数 */
            pthread_mutex_unlock(&g_mutex); /* 释放互斥锁 */
            continue;                     /* 跳过此图片继续下一张 */
        }
        
        pthread_mutex_unlock(&g_mutex);   /* 释放互斥锁 */

        /* 处理单张图片 */
        success = processImage(i);        /* 调用处理函数，处理当前索引的图片 */
        
        if (success) {
            processedCount++;            /* 增加成功处理计数 */
        } else {
            failedCount++;               /* 增加失败计数 */
        }
        
        /* 无论成功还是失败，继续处理下一张图片 */
        /* 只有在处理被停止时才中断循环 */
        pthread_mutex_lock(&g_mutex);
        if (!g_isProcessing) {
            pthread_mutex_unlock(&g_mutex);
            break;
        }
        pthread_mutex_unlock(&g_mutex);
        
        /* 休眠一小段时间，避免系统负载过高 */
        usleep(20000);                   /* 暂停20毫秒(20,000微秒) */
    }

    /* 处理完成后更新状态 */
    pthread_mutex_lock(&g_mutex);         /* 获取互斥锁 */
    g_isProcessing = false;               /* 设置处理状态为已停止 */
    pthread_mutex_unlock(&g_mutex);       /* 释放互斥锁 */
    
    /* 可以在这里记录处理统计信息 */
    printf("处理完成: 成功处理%d张图片, 失败%d张图片, 跳过%d张已处理图片\n", 
           processedCount, failedCount, skipCount);

    return NULL;                          /* 返回NULL，线程结束 */
}

/***************************************************************************************************
** 函数名称: uploadfile_fun
** 功能描述: 处理用户上传的图片文件
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void uploadfile_fun(Webs * wp)
{
    WebsKey *s;
    WebsUpload *up;
    char *upfile;
    cJSON *response = NULL;
    cJSON *data = NULL;
    char *jsonString = NULL;
    int success = 0;
    
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    response = cJSON_CreateObject();
    initImageDirs();
    
    /* 仅处理POST请求 */
    if (scaselessmatch(wp->method, "POST")) {
        /* 遍历所有上传的文件 */
        for (s = hashFirst(wp->files); s; s = hashNext(wp->files, s)) {
            up = s->content.value.symbol;

            if (!isSupportedImageFile(up->clientFilename)) {
                cJSON_AddBoolToObject(response, "success", 0);
                cJSON_AddStringToObject(response, "message", "unsupported image type");
                continue;
            }
            
            /* 构建目标文件路径 */
            upfile = sfmt("%s/%s", IMAGE_DIR, up->clientFilename);
           
            /* 如果目标文件已存在，先删除 */
            if (access(upfile, F_OK) != -1) {
                unlink(upfile);
            }
            
            /* 使用高效文件复制替代系统命令 */
            if (copyFile(up->filename, upfile)) {
                /* 删除临时文件 */
                unlink(up->filename);
                removeProcessedImageFor(up->clientFilename);
                upsertImageRecord(up->clientFilename, false);
                saveImageStates();
                success = 1;
                
                /* 添加文件信息到JSON响应 */
                data = cJSON_CreateObject();
                cJSON_AddStringToObject(data, "filename", up->clientFilename);
                cJSON_AddStringToObject(data, "path", upfile);
                cJSON_AddItemToObject(response, "data", data);
            } else {
                error("Cannot copy uploaded file: %s to %s", up->filename, upfile);
                cJSON_AddBoolToObject(response, "success", 0);
                cJSON_AddStringToObject(response, "message", "upload failed");
            }
            wfree(upfile);
        }
        
        /* 设置成功状态 */
        cJSON_AddBoolToObject(response, "success", success);
        if (success) {
            cJSON_AddStringToObject(response, "message", "upload successful");
        }
    } else {
        cJSON_AddBoolToObject(response, "success", 0);
        cJSON_AddStringToObject(response, "message", "only POST method is supported");
    }
    
    /* 将JSON对象转换为字符串并发送 */
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    /* 释放资源 */
    free(jsonString);
    cJSON_Delete(response);
    
    websDone(wp);
}

/***************************************************************************************************
** 函数名称: startProcessing_fun
** 功能描述: 开始批量处理图片
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void startProcessing_fun(Webs * wp)
{
    char cmd[256];                       /* 存储系统命令的缓冲区 */
    char imagePath[256];                 /* 存储图片完整路径 */
    DIR *dir;                            /* 目录流指针 */
    struct dirent *entry;                /* 目录项结构指针 */
    struct stat st;                      /* 文件状态结构 */
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    int error = 0;                       /* 错误计数 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    /* 先停止正在进行的处理 */
    if (g_isProcessing) {                /* 如果当前正在进行处理 */
        /* 等待处理线程结束 */
        pthread_mutex_lock(&g_mutex);    /* 获取互斥锁 */
        g_isProcessing = false;          /* 设置停止标志 */
        pthread_mutex_unlock(&g_mutex);  /* 释放互斥锁 */
        pthread_join(g_processThread, NULL); /* 等待处理线程结束 */
    }
    
    /* 清空图片列表 */
    clearImageList();                    /* 清空之前的图片信息 */
    
    /* 确保目录存在 */
    initImageDirs();                     /* 初始化图片存储目录 */
    clearDirectory(PROCESSED_DIR);       /* 切换模型后强制重新生成处理结果 */
    
    /* 扫描图片目录 */
    dir = opendir(IMAGE_DIR);            /* 打开图片目录 */
    if (!dir) {                          /* 如果目录打开失败 */
        /* 目录打开失败 */
        response = cJSON_CreateObject();  /* 创建JSON响应对象 */
        datas = cJSON_CreateObject();     /* 创建数据对象 */
        cJSON_AddNumberToObject(response, "resp_code", -1); /* 添加错误码 */
        cJSON_AddStringToObject(response, "resp_msg", "无法打开图片目录"); /* 添加错误信息 */
        cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
        cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
        
        jsonString = cJSON_Print(response); /* 将JSON对象转换为字符串 */
        websWrite(wp, "%s", jsonString);    /* 将JSON字符串写入响应 */
        
        cJSON_Delete(response);             /* 释放JSON对象 */
        free(jsonString);                   /* 释放JSON字符串 */
        websDone(wp);                       /* 完成响应处理 */
        return;                             /* 返回，结束函数 */
    }
    
    /* 遍历目录下的所有图片文件 */
    g_imageCount = 0;                       /* 初始化图片计数为0 */
    while ((entry = readdir(dir)) != NULL && g_imageCount < MAX_IMAGES) { /* 读取目录项 */
        /* 忽略.和..目录 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;                        /* 跳过当前和父目录 */
        }
        
        /* 构建完整路径 */
        sprintf(imagePath, "%s/%s", IMAGE_DIR, entry->d_name); /* 构建图片完整路径 */
        
        /* 使用stat检查是否为常规文件 */
        if (stat(imagePath, &st) == 0 && S_ISREG(st.st_mode)) { /* 如果是常规文件 */
            /* 只处理常见图片格式 */
            char *ext = strrchr(entry->d_name, '.'); /* 获取文件扩展名 */
            if (isSupportedImageFile(entry->d_name)) {
                /* 添加到图片列表 */
                strncpy(g_images[g_imageCount].clientFilename, entry->d_name, 
                        sizeof(g_images[g_imageCount].clientFilename) - 1);
                strncpy(g_images[g_imageCount].filename, imagePath, 
                        sizeof(g_images[g_imageCount].filename) - 1);
                g_images[g_imageCount].processed = false;
                
                g_imageCount++;
            }
        }
    }
    closedir(dir); /* 关闭目录 */
    
    /* 首次扫描完成后，保存状态到文件 */
    if (!saveImageStates()) {
        logmsg(1, "警告: 无法保存图像状态");
        error++;                         /* 增加错误计数 */
    }
    
    logmsg(2, "初始化完成，共发现 %d 张图片", g_imageCount);
    /* 启动处理线程 */
    pthread_mutex_lock(&g_mutex); /* 获取互斥锁 */
    g_isProcessing = true;        /* 设置处理状态为正在处理 */
    g_currentImageIndex = 0;      /* 初始化当前处理索引为0 */
    pthread_mutex_unlock(&g_mutex); /* 释放互斥锁 */
    
    if (pthread_create(&g_processThread, NULL, processImagesThread, NULL) != 0) { /* 创建处理线程 */
        /* 创建线程失败 */
        pthread_mutex_lock(&g_mutex); /* 获取互斥锁 */
        g_isProcessing = false;      /* 设置处理状态为未处理 */
        pthread_mutex_unlock(&g_mutex); /* 释放互斥锁 */
        
        response = cJSON_CreateObject(); /* 创建JSON响应对象 */
        datas = cJSON_CreateObject();    /* 创建数据对象 */
        cJSON_AddNumberToObject(response, "resp_code", -1); /* 添加错误码 */
        cJSON_AddStringToObject(response, "resp_msg", "启动处理线程失败"); /* 添加错误信息 */
        cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
        cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
    } else {
        /* 线程创建成功 */
        response = cJSON_CreateObject(); /* 创建JSON响应对象 */
        datas = cJSON_CreateObject();    /* 创建数据对象 */
        cJSON_AddNumberToObject(response, "resp_code", 0); /* 添加成功码 */
        cJSON_AddStringToObject(response, "resp_msg", "开始并行处理图片 (使用 3 个工作线程)"); /* 添加成功信息 */
        cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
        cJSON_AddNumberToObject(datas, "totalImages", g_imageCount); /* 添加总图片数 */
        cJSON_AddNumberToObject(datas, "threads", WORKER_THREADS); /* 添加线程数 */
        cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
    }
    
    jsonString = cJSON_Print(response); /* 将JSON对象转换为字符串 */
    websWrite(wp, "%s", jsonString);    /* 将JSON字符串写入响应 */
    
    cJSON_Delete(response);             /* 释放JSON对象 */
    free(jsonString);                   /* 释放JSON字符串 */
    websDone(wp);                       /* 完成响应处理 */
}

/***************************************************************************************************
** 函数名称: stopProcessing_fun
** 功能描述: 停止批量处理图片
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void stopProcessing_fun(Webs * wp)
{
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    int processedCount = 0;              /* 已处理图片计数 */
    int i;                               /* 循环索引 */
    bool wasProcessing = false;          /* 是否之前是处理状态 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    /* 停止处理 */
    pthread_mutex_lock(&g_mutex);        /* 获取互斥锁 */
    wasProcessing = g_isProcessing;      /* 记录当前处理状态 */
    g_isProcessing = false;              /* 设置处理标志为停止 */
    pthread_mutex_unlock(&g_mutex);      /* 释放互斥锁 */
    
    /* 等待主处理线程结束 - 但添加错误处理 */
    if (wasProcessing && g_processThread) {
        /* 如果之前在处理，并且线程存在 */
        int joinResult = pthread_join(g_processThread, NULL);
        if (joinResult != 0) {
            printf("警告: 等待处理线程结束失败，错误码: %d\n", joinResult);
            /* 继续执行，不返回错误，因为已经设置了停止标志 */
        }
        
        /* 重置线程ID */
        g_processThread = 0;
    }
    
    /* 统计实际已处理的图片数量 */
    pthread_mutex_lock(&g_mutex);        /* 获取互斥锁，保护共享数据 */
    for (i = 0; i < g_imageCount; i++) {
        /* 检查处理后文件是否存在 */
        char processedPath[512];
        sprintf(processedPath, "%s/%s", PROCESSED_DIR, g_images[i].clientFilename);
        
        if (access(processedPath, F_OK) != -1) {
            struct stat st;
            if (stat(processedPath, &st) == 0 && st.st_size > 0) {
                g_images[i].processed = true;  /* 标记为已处理 */
                processedCount++;              /* 增加已处理计数 */
            } else {
                g_images[i].processed = false; /* 文件不存在或大小为0，标记为未处理 */
            }
        } else {
            g_images[i].processed = false;     /* 文件不存在，标记为未处理 */
        }
    }
    pthread_mutex_unlock(&g_mutex);      /* 释放互斥锁 */
    
    /* 保存状态到文件 */
    saveImageStates();
    
    response = cJSON_CreateObject();     /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();        /* 创建数据对象 */
    cJSON_AddNumberToObject(response, "resp_code", 0); /* 添加成功码 */
    
    if (wasProcessing) {
        cJSON_AddStringToObject(response, "resp_msg", "已停止处理"); /* 添加成功信息 */
    } else {
        cJSON_AddStringToObject(response, "resp_msg", "已重置处理状态"); /* 如果本来就没有在处理 */
    }
    
    cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
    
    /* 添加已处理图片数量到响应 */
    cJSON_AddNumberToObject(datas, "processedCount", processedCount);
    cJSON_AddNumberToObject(datas, "totalCount", g_imageCount);
    cJSON_AddBoolToObject(datas, "wasProcessing", wasProcessing);
    
    cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
    
    jsonString = cJSON_Print(response);  /* 将JSON对象转换为字符串 */
    websWrite(wp, "%s", jsonString);     /* 将JSON字符串写入响应 */
    
    printf("已处理停止请求: 处理状态=%s, 已处理图片=%d, 总图片=%d\n", 
           wasProcessing ? "是" : "否", processedCount, g_imageCount);
    
    cJSON_Delete(response);              /* 释放JSON对象 */
    free(jsonString);                    /* 释放JSON字符串 */
    websDone(wp);                        /* 完成响应处理 */
}

/***************************************************************************************************
** 函数名称: getProgress_fun
** 功能描述: 获取处理进度
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void getProgress_fun(Webs * wp)
{
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    cJSON *processedNames = NULL;        /* 已处理图片名称数组 */
    char *jsonString = NULL;             /* JSON字符串 */
    int processedCount = 0;              /* 已处理图片计数 */
    int pendingCount = 0;                /* 待处理图片计数 */
    int skipCount = 0;                   /* 跳过的已处理图片计数 */
    int i;                               /* 循环索引 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    pthread_mutex_lock(&g_mutex);        /* 获取互斥锁，保护共享数据 */
    
    /* 创建已处理图片名称的JSON数组 */
    processedNames = cJSON_CreateArray();
    
    /* 统计已处理的图片数量和跳过的图片数量 */
    for (i = 0; i < g_imageCount; i++) {
        if (g_images[i].processed) {     /* 如果图片已处理 */
            processedCount++;            /* 增加已处理计数 */
            
            /* 将已处理图片的名称添加到数组 */
            cJSON_AddItemToArray(processedNames, cJSON_CreateString(g_images[i].clientFilename));
            
            if (i <= g_currentImageIndex) {
                skipCount++;             /* 如果小于等于当前索引，说明是被跳过的已处理图片 */
            }
        } else {
            pendingCount++;              /* 未处理的图片 */
        }
    }
    
    response = cJSON_CreateObject();     /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();        /* 创建数据对象 */
    cJSON_AddNumberToObject(response, "resp_code", 0); /* 添加成功码 */
    cJSON_AddStringToObject(response, "resp_msg", "成功"); /* 添加成功信息 */
    cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
    
    cJSON_AddNumberToObject(datas, "totalImages", g_imageCount); /* 添加总图片数 */
    cJSON_AddNumberToObject(datas, "processedImages", processedCount); /* 添加已处理图片数 */
    cJSON_AddNumberToObject(datas, "pendingImages", pendingCount); /* 添加待处理图片数 */
    cJSON_AddNumberToObject(datas, "skippedImages", skipCount); /* 添加跳过的图片数 */
    cJSON_AddNumberToObject(datas, "currentIndex", g_currentImageIndex); /* 添加当前处理索引 */
    cJSON_AddBoolToObject(datas, "isProcessing", g_isProcessing); /* 添加处理状态 */
    
    /* 添加已处理图片名称数组 */
    cJSON_AddItemToObject(datas, "processedImageNames", processedNames);
    
    /* 如果有正在处理的图片，返回其名称 */
    if (g_isProcessing && g_currentImageIndex >= 0 && g_currentImageIndex < g_imageCount) {
        cJSON_AddStringToObject(datas, "currentImage", g_images[g_currentImageIndex].clientFilename); /* 添加当前处理图片名 */
        cJSON_AddBoolToObject(datas, "currentImageProcessed", g_images[g_currentImageIndex].processed); /* 当前图片是否已处理 */
    } else {
        cJSON_AddStringToObject(datas, "currentImage", ""); /* 添加空字符串表示无当前处理图片 */
        cJSON_AddBoolToObject(datas, "currentImageProcessed", false);
    }
    
    cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
    
    pthread_mutex_unlock(&g_mutex);      /* 释放互斥锁 */
    
    jsonString = cJSON_Print(response);  /* 将JSON对象转换为字符串 */
    websWrite(wp, "%s", jsonString);     /* 将JSON字符串写入响应 */
    
    cJSON_Delete(response);              /* 释放JSON对象 */
    free(jsonString);                    /* 释放JSON字符串 */
    websDone(wp);                        /* 完成响应处理 */
}

/***************************************************************************************************
** 函数名称: getImageList_fun
** 功能描述: 获取图片列表
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void getImageList_fun(Webs * wp)
{
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    cJSON *imageList = NULL;             /* 图片列表JSON数组 */
    cJSON *imageObj = NULL;              /* 单个图片JSON对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    int i;                               /* 循环索引 */
    char originalUrl[512];               /* 原始图片URL */
    char processedUrl[512];              /* 处理后图片URL */
    char originalUrlWithVersion[640];    /* 带版本号的原始图片URL */
    char processedUrlWithVersion[640];   /* 带版本号的处理后图片URL */
    int processedCount = 0;              /* 已处理图片数量 */
    int pendingCount = 0;                /* 待处理图片数量 */
    char processedPath[512];             /* 处理后图片路径 */
    struct stat st;                      /* 文件状态 */
    bool statusChanged = false;          /* 状态是否改变标志 */
    DIR *dir;                            /* 目录流指针 */
    struct dirent *entry;                /* 目录项 */
    char imagePath[512];                 /* 图片路径 */
    bool imageFound = false;             /* 是否找到图片 */
    int totalImages = 0;                 /* 总图片数量 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    /* 如果图片计数为0，尝试重新扫描图片目录 */
    if (g_imageCount == 0) {
        /* 打开目录 */
        dir = opendir(IMAGE_DIR);
        if (dir) {
            /* 读取每个图片文件 */
            while ((entry = readdir(dir)) != NULL && totalImages < MAX_IMAGES) {
                /* 忽略.和..目录 */
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                /* 构建完整路径 */
                sprintf(imagePath, "%s/%s", IMAGE_DIR, entry->d_name);
                
                /* 检查文件是否为常规文件 */
                if (stat(imagePath, &st) == 0 && S_ISREG(st.st_mode)) {
                    /* 检查文件扩展名 */
                    char *ext = strrchr(entry->d_name, '.');
                    if (ext && (scaselesscmp(ext, ".jpg") == 0 || 
                              scaselesscmp(ext, ".jpeg") == 0 || 
                              scaselesscmp(ext, ".png") == 0 || 
                              scaselesscmp(ext, ".bmp") == 0)) {
                        
                        /* 检查处理后的文件是否存在 */
                        char processedPath[512];
                        sprintf(processedPath, "%s/%s", PROCESSED_DIR, entry->d_name);
                        
                        /* 查找图片是否已经在数组中 */
                        imageFound = false;
                        for (i = 0; i < g_imageCount; i++) {
                            if (strcmp(g_images[i].clientFilename, entry->d_name) == 0) {
                                imageFound = true;
                                break;
                            }
                        }
                        
                        /* 如果不在数组中，添加到数组 */
                        if (!imageFound && g_imageCount < MAX_IMAGES) {
                            strncpy(g_images[g_imageCount].clientFilename, entry->d_name, 
                                    sizeof(g_images[g_imageCount].clientFilename) - 1);
                            strncpy(g_images[g_imageCount].filename, imagePath, 
                                    sizeof(g_images[g_imageCount].filename) - 1);
                            
                            /* 检查是否已处理 */
                            if (access(processedPath, F_OK) != -1) {
                                struct stat proc_st;
                                if (stat(processedPath, &proc_st) == 0 && proc_st.st_size > 0) {
                                    g_images[g_imageCount].processed = true;
                                } else {
                                    g_images[g_imageCount].processed = false;
                                }
                            } else {
                                g_images[g_imageCount].processed = false;
                            }
                            
                            g_imageCount++;
                            statusChanged = true;
                        }
                        
                        totalImages++;
                    }
                }
            }
            
            closedir(dir);
            
            /* 如果发现新图片，保存状态 */
            if (statusChanged) {
                saveImageStates();
            }
        }
    }
    
    pthread_mutex_lock(&g_mutex);        /* 获取互斥锁，保护共享数据 */
    
    /* 在返回前先验证每个图片的状态 */
    for (i = 0; i < g_imageCount; i++) {
        /* 构建处理后图片路径 */
        sprintf(processedPath, "%s/%s", PROCESSED_DIR, g_images[i].clientFilename);
        
        /* 检查处理后文件是否存在且大小不为0 */
        bool fileProcessed = false;
        if (access(processedPath, F_OK) != -1) {
            if (stat(processedPath, &st) == 0 && st.st_size > 0) {
                fileProcessed = true;
            }
        }
        
        /* 更新处理状态 */
        if (fileProcessed != g_images[i].processed) {
            g_images[i].processed = fileProcessed;
            statusChanged = true;  /* 标记状态已更改 */
        }
        
        /* 统计处理状态 */
        if (g_images[i].processed) {
            processedCount++;
        } else {
            pendingCount++;
        }
    }
    
    /* 如果状态有变化，保存状态文件 */
    if (statusChanged) {
        pthread_mutex_unlock(&g_mutex);  /* 临时释放锁以避免死锁 */
        saveImageStates();
        pthread_mutex_lock(&g_mutex);    /* 重新获取锁 */
    }
    
    response = cJSON_CreateObject();     /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();        /* 创建数据对象 */
    imageList = cJSON_CreateArray();     /* 创建图片列表数组 */
    
    /* 添加所有图片信息 */
    for (i = 0; i < g_imageCount; i++) {
        imageObj = cJSON_CreateObject(); /* 创建单个图片对象 */
        
        /* 构建原始图片URL */
        sprintf(originalUrl, "%s/%s", IMAGE_URL_PREFIX, g_images[i].clientFilename); /* 原始图片URL */
        cJSON_AddStringToObject(imageObj, "name", g_images[i].clientFilename); /* 添加图片名称 */
        sprintf(imagePath, "%s/%s", IMAGE_DIR, g_images[i].clientFilename);
        if (stat(imagePath, &st) == 0) {
            snprintf(originalUrlWithVersion, sizeof(originalUrlWithVersion), "%s?t=%ld",
                originalUrl, (long) st.st_mtime);
            cJSON_AddStringToObject(imageObj, "originalUrl", originalUrlWithVersion); /* 添加原始图片URL */
        } else {
            cJSON_AddStringToObject(imageObj, "originalUrl", originalUrl); /* 添加原始图片URL */
        }
        
        if (g_images[i].processed) {     /* 如果图片已处理 */
            /* 构建处理后图片URL */
            sprintf(processedPath, "%s/%s", PROCESSED_DIR, g_images[i].clientFilename);
            sprintf(processedUrl, "%s/%s", PROCESSED_URL_PREFIX, g_images[i].clientFilename); /* 处理后图片URL */
            if (stat(processedPath, &st) == 0) {
                snprintf(processedUrlWithVersion, sizeof(processedUrlWithVersion), "%s?t=%ld",
                    processedUrl, (long) st.st_mtime);
                cJSON_AddStringToObject(imageObj, "processedUrl", processedUrlWithVersion); /* 添加处理后图片URL */
            } else {
                cJSON_AddStringToObject(imageObj, "processedUrl", processedUrl); /* 添加处理后图片URL */
            }
            cJSON_AddBoolToObject(imageObj, "processed", true); /* 设置处理状态为true */
        } else {
            cJSON_AddStringToObject(imageObj, "processedUrl", ""); /* 添加空URL */
            cJSON_AddBoolToObject(imageObj, "processed", false); /* 设置处理状态为false */
        }
        
        cJSON_AddItemToArray(imageList, imageObj); /* 将图片对象添加到图片列表数组 */
    }
    
    cJSON_AddItemToObject(datas, "images", imageList); /* 将图片列表数组添加到数据对象 */
    cJSON_AddBoolToObject(datas, "isProcessing", g_isProcessing); /* 添加处理状态 */
    cJSON_AddNumberToObject(datas, "processedCount", processedCount); /* 添加已处理图片数量 */
    cJSON_AddNumberToObject(datas, "pendingCount", pendingCount); /* 添加待处理图片数量 */
    cJSON_AddNumberToObject(datas, "totalCount", g_imageCount); /* 添加总图片数量 */
    
    cJSON_AddNumberToObject(response, "resp_code", 0); /* 添加成功码 */
    cJSON_AddStringToObject(response, "resp_msg", "成功"); /* 添加成功信息 */
    cJSON_AddStringToObject(response, "resp_target", "null"); /* 添加目标字段 */
    cJSON_AddItemToObject(response, "datas", datas); /* 将数据对象添加到响应中 */
    
    pthread_mutex_unlock(&g_mutex);      /* 释放互斥锁 */
    
    jsonString = cJSON_Print(response);  /* 将JSON对象转换为字符串 */
    websWrite(wp, "%s", jsonString);     /* 将JSON字符串写入响应 */
    
    cJSON_Delete(response);              /* 释放JSON对象 */
    free(jsonString);                    /* 释放JSON字符串 */
    websDone(wp);                        /* 完成响应处理 */
}

/***************************************************************************************************
** 函数名称: clearAllServerImages_fun
** 功能描述: 清空服务器上的所有图片文件
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void clearAllServerImages_fun(Webs * wp)
{
    int originalResult = 0;              /* 原始图片目录清理结果 */
    int processedResult = 0;             /* 处理后图片目录清理结果 */
    cJSON *response = NULL;              /* JSON响应对象 */
    cJSON *datas = NULL;                 /* JSON数据对象 */
    char *jsonString = NULL;             /* JSON字符串 */
    
    websSetStatus(wp, 200);              /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);         /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);             /* 结束HTTP头部写入 */
    
    /* 停止正在进行的处理 */
    if (g_isProcessing) {
        pthread_mutex_lock(&g_mutex);    /* 获取互斥锁 */
        g_isProcessing = false;          /* 设置处理标志为停止 */
        pthread_mutex_unlock(&g_mutex);  /* 释放互斥锁 */
        
        /* 等待处理线程结束 */
        pthread_join(g_processThread, NULL);
    }
    
    /* 清空原始图片目录 */
    originalResult = clearDirectory(IMAGE_DIR);
    
    /* 清空处理后图片目录 */
    processedResult = clearDirectory(PROCESSED_DIR);
    
    /* 清空内存中的图片列表 */
    clearImageList();
    
    /* 保存空的状态文件 */
    saveImageStates();
    
    response = cJSON_CreateObject();     /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();        /* 创建数据对象 */
    
    if (originalResult == 0 && processedResult == 0) {
        /* 清理成功 */
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "成功清空所有图片文件");
        
        /* 添加清空后的计数 */
        cJSON_AddNumberToObject(datas, "processedCount", 0);
        cJSON_AddNumberToObject(datas, "pendingCount", 0);
        cJSON_AddNumberToObject(datas, "totalCount", 0);
    } else {
        /* 清理失败 */
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "清空图片文件时发生错误");
        cJSON_AddNumberToObject(datas, "originalResult", originalResult);
        cJSON_AddNumberToObject(datas, "processedResult", processedResult);
    }
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/**
 * @brief 清空指定目录中的所有文件
 * @param dirPath 需要清空的目录路径
 * @return 成功返回0，失败返回非0值
 */
static int clearDirectory(const char *dirPath)
{
    DIR *dir;                /* 目录流 */
    struct dirent *entry;    /* 目录项 */
    char filePath[512];      /* 文件完整路径 */
    int result = 0;          /* 操作结果 */
    
    /* 打开目录 */
    dir = opendir(dirPath);
    if (!dir) {
        return -1;  /* 无法打开目录 */
    }
    
    /* 遍历目录中的所有文件 */
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过.和..目录 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* 构建完整文件路径 */
        snprintf(filePath, sizeof(filePath), "%s/%s", dirPath, entry->d_name);
        
        /* 删除文件 */
        if (unlink(filePath) != 0) {
            result = -1;  /* 删除失败，标记错误 */
            /* 继续尝试删除其他文件，不立即返回 */
        }
    }
    
    closedir(dir);
    return result;
}

/***************************************************************************************************
** 函数名称: setModelPath_fun
** 功能描述: 设置模型路径
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void setModelPath_fun(Webs * wp)
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
        /* 如果没有提供参数，返回当前模型路径 */
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "当前模型路径");
        cJSON_AddStringToObject(datas, "modelPath", g_modelPath);
    } else {
        /* 安全性检查：防止路径遍历攻击 */
        if (strstr(modelPath, "..") != NULL || strstr(modelPath, "/") != NULL || strstr(modelPath, "\\") != NULL) {
            /* 检测到非法路径 */
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "非法的模型路径");
            cJSON_AddStringToObject(datas, "modelPath", g_modelPath); /* 返回当前值而非提交值 */
    } else {
        char fullPath[512];
            /* 创建模型路径的可修改副本并限制长度 */
            char modelPathCopy[256];
            strncpy(modelPathCopy, modelPath, sizeof(modelPathCopy) - 1);
            modelPathCopy[sizeof(modelPathCopy) - 1] = '\0'; // 确保字符串结束
            
            /* 使用安全的字符串函数 */
            snprintf(fullPath, sizeof(fullPath), "%s/%s", MODEL_DIR, modelPathCopy);
        
            printf("检查模型文件: %s\n", fullPath);
            
        /* 检查模型文件是否存在 */
        if (access(fullPath, F_OK) == -1) {
            /* 模型文件不存在 */
            cJSON_AddNumberToObject(response, "resp_code", -1);
            cJSON_AddStringToObject(response, "resp_msg", "模型文件不存在");
                cJSON_AddStringToObject(datas, "modelPath", g_modelPath); /* 返回当前值而非提交值 */
        } else {
            /* 模型文件存在，设置新的模型路径 */
            pthread_mutex_lock(&g_mutex);    /* 获取互斥锁 */
                strncpy(g_modelPath, modelPathCopy, sizeof(g_modelPath) - 1);
            g_modelPath[sizeof(g_modelPath) - 1] = '\0';  /* 确保字符串结束 */
            pthread_mutex_unlock(&g_mutex);  /* 释放互斥锁 */
            
            cJSON_AddNumberToObject(response, "resp_code", 0);
            cJSON_AddStringToObject(response, "resp_msg", "成功设置模型路径");
            cJSON_AddStringToObject(datas, "modelPath", g_modelPath);
                
                printf("模型已切换为: %s\n", g_modelPath);
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
** 函数名称: getAvailableModels_fun
** 功能描述: 获取可用模型列表
** 输　入  : wp 
** 输　出  : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void getAvailableModels_fun(Webs * wp)
{
    DIR *dir;                              /* 目录流指针 */
    struct dirent *entry;                  /* 目录项结构指针 */
    cJSON *response = NULL;                /* JSON响应对象 */
    cJSON *datas = NULL;                   /* JSON数据对象 */
    cJSON *modelList = NULL;               /* 模型列表JSON数组 */
    cJSON *modelObj = NULL;                /* 单个模型JSON对象 */
    char *jsonString = NULL;               /* JSON字符串 */
    
    websSetStatus(wp, 200);                /* 设置HTTP响应状态码为200(成功) */
    websWriteHeaders(wp, -1, 0);           /* 写入HTTP响应头，-1表示自动计算内容长度 */
    websWriteHeader(wp, "Content-Type", "application/json"); /* 设置内容类型为JSON */
    websWriteEndHeaders(wp);               /* 结束HTTP头部写入 */
    
    response = cJSON_CreateObject();       /* 创建JSON响应对象 */
    datas = cJSON_CreateObject();          /* 创建数据对象 */
    modelList = cJSON_CreateArray();       /* 创建模型列表数组 */
    
    resolveCurrentModel(g_modelPath, sizeof(g_modelPath), true);

    /* 扫描模型目录 */
    dir = opendir(MODEL_DIR);
    if (!dir) {
        /* 目录打开失败 */
        cJSON_AddNumberToObject(response, "resp_code", -1);
        cJSON_AddStringToObject(response, "resp_msg", "无法打开模型目录");
    } else {
        /* 遍历目录下的所有.rknn文件 */
        while ((entry = readdir(dir)) != NULL) {
            /* 忽略.和..目录 */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            /* 只添加.rknn文件 */
            if (isModelFile(entry->d_name)) {
                modelObj = cJSON_CreateObject();
                cJSON_AddStringToObject(modelObj, "name", entry->d_name);
                
                /* 检查是否为当前选择的模型 */
                if (strcmp(entry->d_name, g_modelPath) == 0) {
                    cJSON_AddBoolToObject(modelObj, "selected", true);
                } else {
                    cJSON_AddBoolToObject(modelObj, "selected", false);
                }
                
                cJSON_AddItemToArray(modelList, modelObj);
            }
        }
        closedir(dir);
        
        cJSON_AddNumberToObject(response, "resp_code", 0);
        cJSON_AddStringToObject(response, "resp_msg", "成功获取模型列表");
    }
    
    /* 添加当前选择的模型 */
    cJSON_AddStringToObject(datas, "currentModel", g_modelPath);
    cJSON_AddItemToObject(datas, "models", modelList);
    
    cJSON_AddStringToObject(response, "resp_target", "null");
    cJSON_AddItemToObject(response, "datas", datas);
    
    jsonString = cJSON_Print(response);
    websWrite(wp, "%s", jsonString);
    
    cJSON_Delete(response);
    free(jsonString);
    websDone(wp);
}

/**
 * @brief 确保目录具有适当的读写权限
 * @param dirPath 需要设置权限的目录路径
 * @param mode 要设置的权限模式
 * @return 成功返回0，失败返回非0值
 */
static int ensureDirectoryPermissions(const char *dirPath, mode_t mode) {
    /* 先测试目录是否已具有所需权限 */
    if (access(dirPath, R_OK | W_OK) == 0) {
        /* 目录已具有读写权限，无需更改 */
        return 0;
    }
    
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
    
    return 0;
}

/**
 * 初始化图像处理系统
 * 扫描现有图像、创建必要目录
 * @return 0表示成功，非0表示部分失败
 */
int initImageProcessingSystem(void)
{
    char imagePath[256];                 /* 存储图片完整路径 */
    DIR *dir;                            /* 目录流指针 */
    struct dirent *entry;                /* 目录项结构指针 */
    struct stat st;                      /* 文件状态结构 */
    int errors = 0;                      /* 错误计数 */
    
    logmsg(2, "初始化图片处理系统...");
    
    /* 确保目录存在 */
    initImageDirs();                     /* 初始化图片存储目录 */
    
    /* 确保目录具有适当的权限 */
    if (ensureDirectoryPermissions(IMAGE_DIR, 0755) != 0) {
        logmsg(1, "警告: 无法确保图片目录权限: %s", IMAGE_DIR);
        errors++;
    } else {
        logmsg(2, "已确保图片目录具有读写权限: %s", IMAGE_DIR);
    }
    
    if (ensureDirectoryPermissions(PROCESSED_DIR, 0755) != 0) {
        logmsg(1, "警告: 无法确保处理后图片目录权限: %s", PROCESSED_DIR);
        errors++;
    } else {
        logmsg(2, "已确保处理后图片目录具有读写权限: %s", PROCESSED_DIR);
    }
    
    /* 确保模型目录具有读取权限 */
    if (ensureDirectoryPermissions(MODEL_DIR, 0755) != 0) {
        logmsg(1, "警告: 无法确保模型目录权限: %s", MODEL_DIR);
        errors++;
    } else {
        logmsg(2, "已确保模型目录具有读写权限: %s", MODEL_DIR);
    }

    if (!resolveCurrentModel(g_modelPath, sizeof(g_modelPath), true)) {
        logmsg(1, "警告: 当前没有可用的图片模型，将在处理时使用通用回退识别");
    }
    
    /* 清空图片列表 */
    clearImageList();                    /* 清空之前的图片信息 */
    
    /* 尝试从状态文件加载 */
    if (loadImageStates()) {
        logmsg(2, "成功从状态文件加载了 %d 张图片的信息", g_imageCount);
        return errors;  /* 成功加载，返回错误计数（可能不为0，如果权限设置有问题） */
    }
    
    /* 若状态文件不存在或加载失败，则扫描图片目录 */
    dir = opendir(IMAGE_DIR);            /* 打开图片目录 */
    if (!dir) {                          /* 如果目录打开失败 */
        logmsg(1, "初始化失败: 无法打开图片目录 %s, 错误: %s", IMAGE_DIR, strerror(errno));
        return -1;                       /* 返回-1表示失败 */
    }
    
    /* 遍历目录下的所有图片文件 */
    g_imageCount = 0;                       /* 初始化图片计数为0 */
    
    logmsg(2, "扫描目录: %s", IMAGE_DIR);
    
    while ((entry = readdir(dir)) != NULL && g_imageCount < MAX_IMAGES) { /* 读取目录项 */
        /* 忽略.和..目录 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;                        /* 跳过当前和父目录 */
        }
        
        /* 构建完整路径 - 使用snprintf防止缓冲区溢出 */
        snprintf(imagePath, sizeof(imagePath), "%s/%s", IMAGE_DIR, entry->d_name);
        
        /* 使用stat检查是否为常规文件 */
        if (stat(imagePath, &st) == 0 && S_ISREG(st.st_mode)) { /* 如果是常规文件 */
            /* 只处理常见图片格式 */
            char *ext = strrchr(entry->d_name, '.'); /* 获取文件扩展名 */
            if (ext && (scaselesscmp(ext, ".jpg") == 0 || scaselesscmp(ext, ".jpeg") == 0 || 
                        scaselesscmp(ext, ".png") == 0 || scaselesscmp(ext, ".bmp") == 0)) {
                
                /* 检查该图片是否已经处理过 - 使用snprintf防止缓冲区溢出 */
                char processedPath[512];
                snprintf(processedPath, sizeof(processedPath), "%s/%s", PROCESSED_DIR, entry->d_name);
                
                /* 添加到图片列表 - 确保字符串正确终止 */
                strncpy(g_images[g_imageCount].clientFilename, entry->d_name, sizeof(g_images[g_imageCount].clientFilename) - 1);
                g_images[g_imageCount].clientFilename[sizeof(g_images[g_imageCount].clientFilename) - 1] = '\0';
                
                strncpy(g_images[g_imageCount].filename, imagePath, sizeof(g_images[g_imageCount].filename) - 1);
                g_images[g_imageCount].filename[sizeof(g_images[g_imageCount].filename) - 1] = '\0';
                
                /* 如果处理后文件存在但无法获取状态，也计入错误 */
                if (access(processedPath, F_OK) != -1) {
                    struct stat proc_st;
                    if (stat(processedPath, &proc_st) != 0) {
                        logmsg(1, "警告: 无法获取文件状态: %s, 错误: %s", processedPath, strerror(errno));
                        g_images[g_imageCount].processed = false;
                        errors++;
                    } else if (proc_st.st_size > 0) {
                        g_images[g_imageCount].processed = true;
                    } else {
                        g_images[g_imageCount].processed = false;
                        errors++;
                    }
                } else {
                    g_images[g_imageCount].processed = false;
                }
                
                g_imageCount++; /* 增加图片计数 */
            }
        }
    }
    
    closedir(dir); /* 关闭目录 */
    
    /* 首次扫描完成后，保存状态到文件 */
    if (!saveImageStates()) {
        logmsg(1, "警告: 无法保存图像状态");
        errors++;
    }
    
    logmsg(2, "初始化完成，共发现 %d 张图片", g_imageCount);
    
    return errors; /* 返回错误计数，0表示完全成功 */
}
