#ifndef _MODELUPLOAD_H        /* 防止头文件重复包含的宏定义开始 */
#define _MODELUPLOAD_H        /* 定义头文件包含标记 */

#include "goahead.h"          /* 包含GoAhead核心头文件 */
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

/* 定义模型类型常量 */
#define MODEL_TYPE_IMAGE       1        /* 图片处理模型 */
#define MODEL_TYPE_VIDEO       2        /* 视频处理模型 */

/* 定义模型目录路径 */
#define IMAGE_MODEL_DIR       "web/model/image"      /* 图片模型存储目录 */
#define VIDEO_MODEL_DIR       "web/model/video"      /* 视频模型存储目录 */

/**
 * uploadModel_fun - 处理模型上传请求
 * 根据用户选择的处理类型（图片或视频）将模型文件上传到相应目录
 * @param wp: GoAhead web请求结构体
 */
void uploadModel_fun(Webs *wp);

/**
 * deleteModel_fun - 处理模型删除请求
 * 根据用户选择的处理类型（图片或视频）从相应目录删除模型文件
 * @param wp: GoAhead web请求结构体
 */
void deleteModel_fun(Webs *wp);

/**
 * initModelDirectories - 初始化模型目录
 * 确保图片和视频模型目录存在并具有适当权限
 * @return: 成功返回0，失败返回非0
 */
int initModelDirectories(void);

#endif /* _MODELUPLOAD_H */    /* 防止头文件重复包含的宏定义结束 */ 