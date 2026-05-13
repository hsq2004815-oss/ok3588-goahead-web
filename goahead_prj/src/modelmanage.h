#ifndef _MODEL_MANAGE_H
#define _MODEL_MANAGE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "goahead.h"

void importFile_fun(Webs * wp);
void uploadModelChunk_fun(Webs * wp);  /* 处理模型文件分片上传功能，支持大模型文件的分片上传 */
void mergeModelChunks_fun(Webs * wp);  /* 合并模型文件分片功能，将上传的模型分片合并为完整模型 */
int initModelManageSystem(void);

#endif /* _MODEL_MANAGE_H */ 