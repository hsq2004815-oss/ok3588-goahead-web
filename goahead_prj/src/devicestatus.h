#ifndef _DEVICESTATUS_H
#define _DEVICESTATUS_H

#include "goahead.h"

/**
 * overview_fun - 返回设备状态概览信息
 * @param wp: GoAhead web请求结构体
 */
void overview_fun(Webs *wp);

/**
 * syslog_fun - 返回系统日志信息
 * @param wp: GoAhead web请求结构体
 */
void syslog_fun(Webs *wp);

#endif /* _DEVICESTATUS_H */ 