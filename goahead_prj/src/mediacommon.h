#ifndef _MEDIA_COMMON_H
#define _MEDIA_COMMON_H

#include "goahead.h"

/* 媒体类型枚举 */
typedef enum {
    MEDIA_TYPE_IMAGE = 0,
    MEDIA_TYPE_VIDEO = 1
} MediaType;

/* 通用的获取媒体列表函数 */
void getMediaList(Webs *wp, MediaType type);

#endif /* _MEDIA_COMMON_H */ 