/********************************* Includes ***********************************/
#include "goahead.h"        /* GoAhead核心头文件 */
#include "videoprocess1.h"  /* 当前模块的头文件 */
#include "videoprocess.h"   /* 包含此头文件以使用共享的模型变量 */
#include "cJSON.h"          /* JSON解析库 */
#include <stdio.h>          /* 标准输入输出函数 */
#include <stdlib.h>         /* 标准库函数 */
#include <string.h>         /* 字符串操作函数 */
#include <sys/types.h>      /* 系统类型定义 */
#include <sys/stat.h>       /* 文件状态相关 */
#include <fcntl.h>          /* 文件控制操作 */
#include <unistd.h>         /* Unix标准函数 */
#include <errno.h>          /* 错误码定义 */
#include <sys/wait.h>       /* 进程等待相关 */
#include <signal.h>         /* 进程信号处理 */
#include <pthread.h>        /* POSIX线程库，提供pthread_mutex_t定义 */

/********************************* Defines ************************************/
#define VIDEO_TMP_DIR "web/tmp"
#define VIDEO_SHOW_DIR "web"
#define VIDEO_FRAME "frame.jpg"
#define VIDEO_RESULT "result.jpg"
#define BUFFER_SIZE 4096
#define MODEL_DIR "web/model/video"       /* 模型目录路径，与videoprocess.c中保持一致 */

static const unsigned char base64_table[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static pthread_mutex_t g_realtime_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_realtime_worker_pid = -1;
static FILE *g_realtime_worker_in = NULL;
static FILE *g_realtime_worker_out = NULL;

static void setup_realtime_child_env(void) {
    setenv("DISPLAY", ":0", 1);
    setenv("PYTHONPATH", "/usr/local/lib/python3.9/site-packages", 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "video_codec=h264", 1);
}

static void close_realtime_worker_locked(void) {
    int status = 0;

    if (g_realtime_worker_in) {
        fclose(g_realtime_worker_in);
        g_realtime_worker_in = NULL;
    }
    if (g_realtime_worker_out) {
        fclose(g_realtime_worker_out);
        g_realtime_worker_out = NULL;
    }
    if (g_realtime_worker_pid > 0) {
        pid_t wait_result = waitpid(g_realtime_worker_pid, &status, WNOHANG);
        if (wait_result == 0) {
            kill(g_realtime_worker_pid, SIGTERM);
            waitpid(g_realtime_worker_pid, &status, 0);
        }
        g_realtime_worker_pid = -1;
    }
}

static int start_realtime_worker_locked(char *error_buf, size_t error_buf_size) {
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    pid_t pid;

    if (g_realtime_worker_pid > 0 && g_realtime_worker_in && g_realtime_worker_out) {
        return 0;
    }

    close_realtime_worker_locked();

    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        snprintf(error_buf, error_buf_size, "pipe failed: %s", strerror(errno));
        if (to_child[0] >= 0) close(to_child[0]);
        if (to_child[1] >= 0) close(to_child[1]);
        if (from_child[0] >= 0) close(from_child[0]);
        if (from_child[1] >= 0) close(from_child[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        snprintf(error_buf, error_buf_size, "fork failed: %s", strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);

        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);

        setup_realtime_child_env();
        if (chdir(MODEL_DIR) != 0) {
            exit(1);
        }

        execlp("python3", "python3", "test1.py", "--worker", NULL);
        exit(1);
    }

    close(to_child[0]);
    close(from_child[1]);

    g_realtime_worker_in = fdopen(to_child[1], "w");
    g_realtime_worker_out = fdopen(from_child[0], "r");
    if (!g_realtime_worker_in || !g_realtime_worker_out) {
        snprintf(error_buf, error_buf_size, "fdopen failed");
        if (g_realtime_worker_in) {
            fclose(g_realtime_worker_in);
            g_realtime_worker_in = NULL;
        } else if (to_child[1] >= 0) {
            close(to_child[1]);
        }
        if (g_realtime_worker_out) {
            fclose(g_realtime_worker_out);
            g_realtime_worker_out = NULL;
        } else if (from_child[0] >= 0) {
            close(from_child[0]);
        }
        g_realtime_worker_pid = pid;
        close_realtime_worker_locked();
        return -1;
    }

    setvbuf(g_realtime_worker_in, NULL, _IOLBF, 0);
    setvbuf(g_realtime_worker_out, NULL, _IOLBF, 0);
    g_realtime_worker_pid = pid;
    return 0;
}

static int run_single_frame_process(const char *frame_path, const char *result_path, const char *json_path, const char *currentModel) {
    int status = -1;
    pid_t child_pid = fork();

    if (child_pid == -1) {
        return -1;
    } else if (child_pid == 0) {
        setup_realtime_child_env();
        if (chdir(MODEL_DIR) != 0) {
            exit(1);
        }

        execlp("python3", "python3", "test1.py",
               "--source", frame_path,
               "--output", result_path,
               "--json", json_path,
               "--model", currentModel,
               NULL);
        exit(1);
    }

    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static int run_realtime_worker_locked(const char *frame_path, const char *result_path, const char *json_path,
                                      const char *currentModel, char *error_buf, size_t error_buf_size) {
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *code_obj = NULL;
    cJSON *msg_obj = NULL;
    char *request_text = NULL;
    char response_text[1024];
    int result = -1;

    if (start_realtime_worker_locked(error_buf, error_buf_size) != 0) {
        return -1;
    }

    request = cJSON_CreateObject();
    if (!request) {
        snprintf(error_buf, error_buf_size, "failed to build worker request");
        return -1;
    }

    cJSON_AddStringToObject(request, "input", frame_path);
    cJSON_AddStringToObject(request, "output", result_path);
    cJSON_AddStringToObject(request, "json", json_path);
    cJSON_AddStringToObject(request, "model", currentModel);
    cJSON_AddNumberToObject(request, "conf_thresh", 0.25);

    request_text = cJSON_PrintUnformatted(request);
    if (!request_text) {
        snprintf(error_buf, error_buf_size, "failed to serialize worker request");
        goto cleanup;
    }

    if (fprintf(g_realtime_worker_in, "%s\n", request_text) < 0 || fflush(g_realtime_worker_in) != 0) {
        snprintf(error_buf, error_buf_size, "failed to send worker request");
        close_realtime_worker_locked();
        goto cleanup;
    }

    if (!fgets(response_text, sizeof(response_text), g_realtime_worker_out)) {
        snprintf(error_buf, error_buf_size, "worker response missing");
        close_realtime_worker_locked();
        goto cleanup;
    }

    response = cJSON_Parse(response_text);
    if (!response) {
        snprintf(error_buf, error_buf_size, "invalid worker response: %s", response_text);
        close_realtime_worker_locked();
        goto cleanup;
    }

    code_obj = cJSON_GetObjectItem(response, "code");
    if (!cJSON_IsNumber(code_obj) || code_obj->valueint != 0) {
        msg_obj = cJSON_GetObjectItem(response, "msg");
        if (cJSON_IsString(msg_obj) && msg_obj->valuestring) {
            snprintf(error_buf, error_buf_size, "%s", msg_obj->valuestring);
        } else {
            snprintf(error_buf, error_buf_size, "worker returned error");
        }
        goto cleanup;
    }

    result = 0;

cleanup:
    if (request_text) {
        free(request_text);
    }
    if (request) {
        cJSON_Delete(request);
    }
    if (response) {
        cJSON_Delete(response);
    }
    return result;
}

/********************************* Functions ***********************************/

static int copy_file(const char *src, const char *dst) {
    int fd_src, fd_dst;
    char buf[BUFFER_SIZE];
    ssize_t nread;
    int ret = 0;

    fd_src = open(src, O_RDONLY);
    if (fd_src < 0) {
        return -1;
    }

    fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0) {
        close(fd_src);
        return -1;
    }

    while ((nread = read(fd_src, buf, sizeof buf)) > 0) {
        char *out_ptr = buf;
        ssize_t nwritten;

        do {
            nwritten = write(fd_dst, out_ptr, nread);
            if (nwritten < 0) {
                ret = -1;
                break;
            }
            nread -= nwritten;
            out_ptr += nwritten;
        } while (nread > 0);

        if (ret < 0) break;
    }

    if (nread < 0) {
        ret = -1;
    }

    close(fd_src);
    close(fd_dst);
    return ret;
}

static void ensure_directory_exists(const char *path) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);  // 使用0777权限确保所有用户可读写执行
            *p = '/';
        }
    }
    mkdir(tmp, 0777);  // 使用0777权限确保所有用户可读写执行
    // 手动给最终目录设置权限
    chmod(tmp, 0777);
}

static unsigned char *base64_decode(const char *src, size_t len, size_t *out_len) {
    unsigned char dtable[256], *out, *pos, block[4], tmp;
    size_t i, count, olen;
    int pad = 0;

    memset(dtable, 0x80, 256);
    for (i = 0; i < sizeof(base64_table) - 1; i++)
        dtable[base64_table[i]] = (unsigned char) i;
    dtable['='] = 0;

    count = 0;
    for (i = 0; i < len; i++) {
        if (dtable[src[i]] != 0x80)
            count++;
    }

    if (count == 0 || count % 4)
        return NULL;

    olen = count / 4 * 3;
    pos = out = malloc(olen);
    if (out == NULL)
        return NULL;

    count = 0;
    for (i = 0; i < len; i++) {
        tmp = dtable[src[i]];
        if (tmp == 0x80)
            continue;

        if (src[i] == '=')
            pad++;
        block[count] = tmp;
        count++;
        if (count == 4) {
            *pos++ = (block[0] << 2) | (block[1] >> 4);
            *pos++ = (block[1] << 4) | (block[2] >> 2);
            *pos++ = (block[2] << 6) | block[3];
            count = 0;
            if (pad) {
                if (pad == 1)
                    pos--;
                else if (pad == 2)
                    pos -= 2;
                else {
                    free(out);
                    return NULL;
                }
                break;
            }
        }
    }

    *out_len = pos - out;
    return out;
}

static int save_base64_image(const char *base64_data, const char *output_path) {
    FILE *fp;
    unsigned char *decoded_data;
    size_t decoded_size;
    const char *data_start;
    
    // 跳过base64头部的data:image/jpeg;base64,
    data_start = strstr(base64_data, ",");
    if (data_start == NULL) {
        data_start = base64_data;
    } else {
        data_start++;
    }
    
    // 解码base64数据
    decoded_data = base64_decode(data_start, strlen(data_start), &decoded_size);
    if (!decoded_data) {
        return -1;
    }
    
    // 保存为文件
    fp = fopen(output_path, "wb");
    if (!fp) {
        free(decoded_data);
        return -1;
    }
    
    fwrite(decoded_data, 1, decoded_size, fp);
    fclose(fp);
    free(decoded_data);
    
    return 0;
}

/***************************************************************************************************
** 函数名称: process_video_frame
** 功能描述: 处理视频帧并返回处理结果
** 输　入  : wp 
** 输　出  : 无
** 全局变量: g_modelPath - 从videoprocess.c共享的当前模型路径
** 调用模块: YOLOv5
***************************************************************************************************/
void process_video_frame(Webs *wp) {
    WebsUpload      *up;
    WebsKey         *key;
    char frame_path[256], result_path[256], json_path[256];
    char relFramePath[512], relResultPath[512], relJsonPath[512];
    char worker_error[256];
    int status = -1; // 处理结果状态
    char *json_content;
    long json_size;
    FILE *json_file;
    char currentModel[256]; // 用于存储当前使用的模型名称
    
    websSetStatus(wp, 200);
    websWriteHeaders(wp, -1, 0);
    websWriteHeader(wp, "Content-Type", "application/json");
    websWriteEndHeaders(wp);
    
    // 只保留最基本的目录准备
    ensure_directory_exists(VIDEO_TMP_DIR);
    ensure_directory_exists(VIDEO_SHOW_DIR);
    chmod(VIDEO_TMP_DIR, 0777);
    chmod(VIDEO_SHOW_DIR, 0777);
    
    // 处理上传的文件
    if (wp->files == 0 || hashFirst(wp->files) == 0) {
        websWrite(wp, "{\"code\": 1, \"msg\": \"No file uploaded\"}");
        websDone(wp);
        return;
    }

    key = hashFirst(wp->files);
    up = key->content.value.symbol;
    
    // 构建文件路径
    snprintf(frame_path, sizeof(frame_path), "%s/%s", VIDEO_TMP_DIR, VIDEO_FRAME);
    snprintf(result_path, sizeof(result_path), "%s/%s", VIDEO_SHOW_DIR, VIDEO_RESULT);
    snprintf(json_path, sizeof(json_path), "%s/result.json", VIDEO_TMP_DIR);

    /* 清理旧的实时检测产物，避免前一帧结果残留影响当前显示 */
    remove(result_path);
    remove(json_path);
    
    // 复制上传的文件
    if (copy_file(up->filename, frame_path) != 0) {
        websWrite(wp, "{\"code\": 1, \"msg\": \"Failed to save frame\"}");
        websDone(wp);
        return;
    }
    
    // 删除临时上传文件
    remove(up->filename);
    
    // 获取当前设置的模型名称，从videoprocess.c中共享的全局变量获取
    pthread_mutex_lock(&g_mutex);
    strncpy(currentModel, g_modelPath, sizeof(currentModel) - 1);
    currentModel[sizeof(currentModel) - 1] = '\0';
    pthread_mutex_unlock(&g_mutex);

    /* 构建相对路径参数，供常驻 worker 和单次回退模式共用 */
    snprintf(relFramePath, sizeof(relFramePath), "../../../%s", frame_path);
    snprintf(relResultPath, sizeof(relResultPath), "../../../%s", result_path);
    snprintf(relJsonPath, sizeof(relJsonPath), "../../../%s", json_path);

    worker_error[0] = '\0';
    pthread_mutex_lock(&g_realtime_worker_mutex);
    status = run_realtime_worker_locked(relFramePath, relResultPath, relJsonPath, currentModel, worker_error, sizeof(worker_error));
    pthread_mutex_unlock(&g_realtime_worker_mutex);

    if (status != 0) {
        logmsg(1, "实时检测 worker 失败，回退单次模式: %s", worker_error[0] ? worker_error : "unknown error");
        status = run_single_frame_process(relFramePath, relResultPath, relJsonPath, currentModel);
    }
    
    if (status != 0) {
        websWrite(wp, "{\"code\": 1, \"msg\": \"Model processing failed - exit status %d\"}", status);
        websDone(wp);
        return;
    }
    
    // 检查结果文件
    if (access(result_path, F_OK) != 0) {
        websWrite(wp, "{\"code\": 1, \"msg\": \"Result not generated\"}");
        websDone(wp);
        return;
    }
    
    // 读取检测结果
    json_content = "[]";
    if (access(json_path, F_OK) == 0) {
        json_file = fopen(json_path, "r");
        if (json_file) {
            fseek(json_file, 0, SEEK_END);
            json_size = ftell(json_file);
            fseek(json_file, 0, SEEK_SET);
            
            char *buffer = (char *)malloc(json_size + 1);
            if (buffer) {
                fread(buffer, 1, json_size, json_file);
                buffer[json_size] = '\0';
                json_content = buffer;
            }
            fclose(json_file);
        }
    }
    
    // 返回成功信息，包含使用的模型信息
    websWrite(wp, "{\"code\": 0, \"msg\": \"Success\", \"processedImage\": \"/result.jpg\", \"objects\": %s, \"usedModel\": \"%s\"}", 
              json_content, currentModel);
    
    if (json_content != "[]") {
        free(json_content);
    }
    
    websDone(wp);
    
    // 清理临时文件
    remove(frame_path);
    remove(json_path);
} 
