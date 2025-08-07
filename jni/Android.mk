LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# 模块名称
LOCAL_MODULE := mem_float_reader

# 源代码文件
LOCAL_SRC_FILES := \
    mem_float_reader.cpp \
    kerneltools.cpp

# 编译选项（启用C++11和异常）
LOCAL_CPPFLAGS := \
    -std=c++11 \
    -Wall \
    -Wextra \
    -DANDROID \
    -fexceptions

# 链接选项（动态库支持）
LOCAL_LDFLAGS := -ldl

# 依赖日志库
LOCAL_LDLIBS := -llog

# 生成可执行文件
include $(BUILD_EXECUTABLE)
