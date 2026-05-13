/********************************* Includes ***********************************/

#include    "goahead.h"
#include    "updatefile.h"
#include    "loginmanage.h"
#include    "videoprocess.h"
#include    "videoprocess1.h"
#include    "modelupload.h"
#include    "devicestatus.h"
#include    "userdb.h"

/********************************* Defines ************************************/

static int finished = 0;

/********************************* Forwards ***********************************/

static void logHeader(void);

/*********************************** Code *************************************/

MAIN(goahead, int argc, char **argv, char **envp)
{
    char     *documents, *endpoints, *endpoint, *route, *auth, *tok;

    route = "web_cfg/route.txt";
    auth = "web_cfg/auth.txt";

    logSetPath("stdout:2");
    documents = ME_GOAHEAD_DOCUMENTS;
    
    if (websOpen(documents, route) < 0) {
        error("Cannot initialize server. Exiting.");
        return -1;
    }
#if ME_GOAHEAD_AUTH
    if (auth && websLoad(auth) < 0) {
        error("Cannot load %s", auth);
        return -1;
    }
#endif
    logHeader();

	endpoints = sclone(ME_GOAHEAD_LISTEN);
	for (endpoint = stok(endpoints, ", \t", &tok); endpoint; endpoint = stok(NULL, ", \t,", &tok)) {
		if (websListen(endpoint) < 0) {
			wfree(endpoints);
			return -1;
		}
	}
	wfree(endpoints);
    logmsg(2, "wait websServiceEvents");
    
    // 注册API处理函数 - 优化分组和注释
    
    // 基础API
    websDefineAction("uploadTest", uploadfile_fun);
    websDefineAction("mylogin", func_action_mylogin);
    websDefineAction("register", func_action_register);
    
    // 图像处理相关API
    logmsg(2, "注册图像处理API...");
    websDefineAction("startProcessing", startProcessing_fun);
    websDefineAction("stopProcessing", stopProcessing_fun);
    websDefineAction("getProgress", getProgress_fun);
    websDefineAction("getImageList", getImageList_fun);
    websDefineAction("clearAllServerImages", clearAllServerImages_fun);
    
    // 模型管理API
    logmsg(2, "注册模型管理API...");
    websDefineAction("setModelPath", setModelPath_fun);
    websDefineAction("getAvailableModels", getAvailableModels_fun);
    websDefineAction("setVideoModel", setVideoModel_fun);
    websDefineAction("getAvailableVideoModels", getAvailableVideoModels_fun);
    websDefineAction("importFile", uploadModel_fun);  // 添加对importFile接口的支持，复用uploadModel_fun处理函数
    websDefineAction("deleteModel", deleteModel_fun); // 注册模型删除API
    
    // 设备状态API
    logmsg(2, "注册设备状态API...");
    websDefineAction("overview", overview_fun);  // 设备状态信息API
    websDefineAction("syslog", syslog_fun);      // 系统日志API
    
    // 视频处理API
    logmsg(2, "注册视频处理API...");
    websDefineAction("uploadVideo", uploadVideo_fun);
    websDefineAction("processVideo", processVideo_fun);
    websDefineAction("getVideoProgress", getVideoProgress_fun);
    websDefineAction("getVideoList", getVideoList_fun);
    websDefineAction("clearAllVideos", clearAllVideos_fun);
    websDefineAction("processVideoFrame", process_video_frame);  // 添加实时视频处理API
    
    
    // 初始化系统
    logmsg(2, "初始化系统...");

    // 初始化登录用户数据库
    if (userdb_init("data/users.db") != 0) {
        logmsg(1, "警告：用户数据库初始化失败，登录功能不可用");
    } else {
        logmsg(2, "用户数据库初始化完成");
    }
    
    // 初始化图片处理系统 - 扫描现有图片
    if (initImageProcessingSystem() != 0) {
        logmsg(1, "警告：图像处理系统初始化不完全");
    } else {
        logmsg(2, "图像处理系统初始化完成");
    }
    
    // 初始化视频处理系统 - 扫描现有视频
    if (initVideoProcessingSystem() != 0) {
        logmsg(1, "警告：视频处理系统初始化不完全");
    } else {
        logmsg(2, "视频处理系统初始化完成");
    }
    
    // 初始化模型目录
    if (initModelDirectories() != 0) {
        logmsg(1, "警告：模型目录初始化不完全");
    } else {
        logmsg(2, "模型目录初始化完成");
    }
    
    // 清理临时上传目录
    // cleanupTempUploadDirs();
    logmsg(2, "临时上传目录清理完成");
    
    logmsg(2, "所有子系统初始化完成，等待客户端连接...");
    
    //等待结束
    websServiceEvents(&finished);
    logmsg(1, "Instructed to exit");
    websClose();

    return 0;
}


static void logHeader(void)
{
    char    home[ME_GOAHEAD_LIMIT_STRING];

    getcwd(home, sizeof(home));
    logmsg(2, "Configuration for %s", ME_TITLE);
    logmsg(2, "---------------------------------------------");
    logmsg(2, "Version:            %s", ME_VERSION);
    logmsg(2, "BuildType:          %s", ME_DEBUG ? "Debug" : "Release");
    logmsg(2, "CPU:                %s", ME_CPU);
    logmsg(2, "OS:                 %s", ME_OS);
    logmsg(2, "Host:               %s", websGetServer());
    logmsg(2, "Directory:          %s", home);
    logmsg(2, "Documents:          %s", websGetDocuments());
    logmsg(2, "Configure:          %s", ME_CONFIG_CMD);
    logmsg(2, "---------------------------------------------");
}
