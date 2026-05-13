#ifndef _UPDATEFILE_H       /* 防止头文件重复包含的宏定义开始 */
#define _UPDATEFILE_H       /* 定义头文件包含标记 */

void uploadfile_fun(Webs * wp);       /* 文件上传处理函数声明，处理用户上传的图片文件 */
void startProcessing_fun(Webs * wp);  /* 开始批量处理图片函数声明，启动图像处理线程 */
void stopProcessing_fun(Webs * wp);   /* 停止批量处理图片函数声明，中断正在进行的处理 */
void getProgress_fun(Webs * wp);      /* 获取处理进度函数声明，返回当前处理状态和进度 */
void getImageList_fun(Webs * wp);     /* 获取图片列表函数声明，返回所有图片及其处理状态 */
void clearAllServerImages_fun(Webs * wp); /* 清空服务器上所有图片函数声明，删除所有图片文件 */
void setModelPath_fun(Webs * wp);     /* 设置模型路径函数声明，选择使用的模型文件 */
void getAvailableModels_fun(Webs * wp); /* 获取可用模型列表函数声明，返回model/image目录下所有.rknn文件 */

int initImageProcessingSystem(void);

#endif /* _UPDATEFILE_H */            /* 防止头文件重复包含的宏定义结束 */
