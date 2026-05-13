#ifndef _MEDIA_CLEANUP_H
#define _MEDIA_CLEANUP_H

#include "goahead.h"
#include "mediacommon.h"

/**
 * 清理指定类型的所有媒体文件
 * @param wp   Web请求结构体
 * @param type 媒体类型，图片或视频
 */
void clearAllMedia(Webs *wp, MediaType type);

#endif /* _MEDIA_CLEANUP_H */ 