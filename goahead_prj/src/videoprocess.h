#ifndef _VIDEOPROCESS_H       /* 防止头文件重复包含的宏定义开始 */
#define _VIDEOPROCESS_H       /* 定义头文件包含标记 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdbool.h>
#include <time.h>

/* 全局变量声明 */
extern char g_modelPath[256];     /* 当前使用的模型路径，声明为外部变量以便共享 */
extern pthread_mutex_t g_mutex;   /* 互斥锁，保护共享数据 */

void uploadVideo_fun(Webs * wp);       /* 视频上传处理功能，接收用户上传的视频文件，并保存到服务器 */

void processVideo_fun(Webs * wp);      /* 视频处理功能，使用AI模型处理指定的视频文件 */

void getVideoList_fun(Webs * wp);      /* 获取视频列表功能，返回服务器上所有已上传和处理的视频信息 */

void getVideoProgress_fun(Webs * wp);  /* 获取视频处理进度，返回当前处理百分比和状态 */

void clearAllVideos_fun(Webs * wp);    /* 清空所有视频功能，删除服务器上所有的视频文件 */

void setVideoModel_fun(Webs * wp);     /* 设置视频处理模型功能，指定用于视频处理的AI模型 */

void getAvailableVideoModels_fun(Webs * wp);  /* 获取可用视频模型功能，返回所有可用的AI模型列表 */

int initVideoProcessingSystem(void);   /* 初始化视频处理系统，扫描现有视频、创建必要目录，0表示成功，非0表示部分失败 */

#endif /* _VIDEOPROCESS_H */            /* 防止头文件重复包含的宏定义结束 */
