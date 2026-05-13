/********************************* Includes ***********************************/
#include    "goahead.h"
#include    "loginmanage.h"
#include    "cJSON.h"
#include    "userdb.h"

/********************************* Defines ************************************/



/********************************* Forwards ***********************************/



/*
    Output an error message and cleanup
 */
PUBLIC void websResponseJs(Webs *wp, int code, char * resp_target, char * datas, char * resp_msg)
{

    char       *buf;
    assert(wp);
    buf = sfmt("{\"resp_code\": %d, \"resp_target\": %s, \"datas\": %s, \"resp_msg\": \"%s\"}",
    		code, resp_target, datas, resp_msg);

    printf("websResponse: %s\n", buf);
    websResponse(wp, 200, buf);
    wfree(buf);
}

/***************************************************************************************************
** 函数名称: func_action_mylogin
** 功能描述: 登录管理
** 输　入    : wp 
** 输　出    : 无
** 全局变量:
** 调用模块:
***************************************************************************************************/
void func_action_mylogin(Webs *wp)
{
    int valid;

    printf("func_action_mylogin run \n");

#if 0
    printf("servp: %s\r\n", wp->input.servp);
    printf("endp: %s\r\n", wp->input.endp);
    printf("endbuf: %s\r\n", wp->input.endbuf);

    char*jsonString=wp->input.servp;    
    char name[256] = {0};
    char pass[256] = {0};
    cJSON *json = NULL;
    cJSON *json_val = NULL;
    cJSON *json_timestamp = NULL;
    json = cJSON_Parse(jsonString);
    if (NULL == json)
    {   
        printf("cJSON_Parse error:%s\n", cJSON_GetErrorPtr());
    }   
    json_val = cJSON_GetObjectItem(json, "username");
    if (cJSON_String == json_val->type)
    {   
        printf("username:%s\n", json_val->valuestring);
    }
    strcat(name, json_val->valuestring);
    json_val = cJSON_GetObjectItem(json, "password");
    if (cJSON_String == json_val->type)
    {   
        printf("password:%s\n", json_val->valuestring);
    }
    strcat(pass, json_val->valuestring);

    cJSON_Delete(json);
#endif
    char * name = websGetVar(wp, "username", "");
    char * pass = websGetVar(wp, "password", "");
    printf("get login user :<%s>\n", name);

    valid = userdb_verify_login(name, pass);
    if (valid == 1) {
    	websResponseJs(wp, 0, "null", "null", "login success");
    } else {
    	websResponseJs(wp, -1, "null", "null", "login error");
    }
    // 返回应答
    websDone(wp);
}

void func_action_register(Webs *wp)
{
    char    message[128];
    char    *name;
    char    *pass;
    int     result;

    name = websGetVar(wp, "username", "");
    pass = websGetVar(wp, "password", "");
    printf("register user :<%s>\n", name);

    result = userdb_register_user(name, pass, message, sizeof(message));
    if (result == 1) {
        websResponseJs(wp, 0, "null", "null", "register success");
    } else {
        websResponseJs(wp, -1, "null", "null", message[0] ? message : "register error");
    }
    websDone(wp);
}
