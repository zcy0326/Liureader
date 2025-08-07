#include "KernelTools.h"
#include "draw.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <android/log.h>
#include <errno.h>       // 新增：定义errno
#include <algorithm>     // 新增：定义std::min

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KernelTools", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KernelTools", __VA_ARGS__)

// 橘子驱动文件描述结构体
typedef struct _copy_filedescription {
    int target;
    char name[15];
} copy_filedescription, *pcopy_filedescription;

NotificationManager* notificationManager = nullptr;

void NotificationManager::addMessage(const char* msg, int type) {
    (void)type;
    LOGI("[通知] %s", msg);
    printf("[通知] %s\n", msg);
}

void init_notification_manager() {
    if (notificationManager == nullptr) {
        notificationManager = new NotificationManager();
    }
}

// 备用读取方法（直接读写/proc/[pid]/mem）
bool read_proc_mem(pid_t pid, uintptr_t addr, void* buffer, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        // 修复：errno需要#include <errno.h>
        LOGE("备用方法：打开/proc/%d/mem失败: %s", pid, strerror(errno));
        return false;
    }

    // 定位到目标地址
    if (lseek(fd, addr, SEEK_SET) == -1) {
        LOGE("备用方法：lseek到0x%lx失败: %s", addr, strerror(errno));
        close(fd);
        return false;
    }

    // 读取数据
    ssize_t bytes_read = read(fd, buffer, size);
    close(fd);
    if (bytes_read != (ssize_t)size) {
        // 修复：ssize_t用%zd，size_t用%zu
        LOGE("备用方法：仅读取到%zd字节（预期%zu）", bytes_read, size);
        return false;
    }
    LOGI("备用方法：成功读取0x%lx，大小%zu字节", addr, size);  // 修复：size_t用%zu
    return true;
}

namespace driver {
    int fd;
    pid_t pid;
    int target = -1;
    bool isBing = false;
    copy_filedescription cf;

    typedef struct _COPY_MEMORY {
        pid_t pid;
        uintptr_t addr;
        void* buffer;
        size_t size;
    } COPY_MEMORY, *PCOPY_MEMORY;

    typedef struct _MODULE_BASE {
        pid_t pid;
        char* name;
        uintptr_t base;
    } MODULE_BASE, *PMODULE_BASE;

    enum OPERATIONS_Bing {
        OP_INIT_KEY_Bing = 0x600,
        OP_READ_MEM_Bing = 0x601,
        OP_WRITE_MEM_Bing = 0x602,
        OP_MODULE_BASE_Bing = 0x603,
    };

    enum OPERATIONS {
        OP_INIT_KEY = 0x800,
        OP_READ_MEM = 0x801,
        OP_WRITE_MEM = 0x802,
        OP_MODULE_BASE = 0x803,
    };

    typedef int (*init)(copy_filedescription* cf);
    typedef char (*Init)(int Target, pid_t gamepid, char* key);
    Init InitialTarget;
    init gettarget;

    void* handle;
    void* handle1;
}

bool driver::init_key(char* key) {
    char buf[0x100];
    strncpy(buf, key, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    if (ioctl(fd, OP_INIT_KEY_Bing, buf) != 0) {
        LOGE("BING驱动密钥初始化失败");
        return false;
    }
    return true;
}

// 驱动读取函数（修复后）
bool driver::read(uintptr_t addr, void* buffer, size_t size) {
    if (!buffer || size == 0) {
        LOGE("读取失败：无效缓冲区（%p）或大小（%zu）", buffer, size);
        return false;
    }
    if (pid <= 0) {
        LOGE("读取失败：目标PID无效（%d）", pid);
        return false;
    }

    // 初始化缓冲区为0xAA（用于检测是否被驱动写入）
    memset(buffer, 0xAA, size);

    COPY_MEMORY cm;
    cm.pid = pid;  // 强制绑定目标PID
    cm.addr = addr;
    cm.buffer = buffer;
    cm.size = size;

    LOGI("\n=== 驱动读取请求 ===");
    LOGI("PID: %d, 地址: 0x%lx, 大小: %zu字节", pid, addr, size);
    LOGI("驱动类型: %s", isBing ? "BING" : "橘子");

    int ret = -1;
    if (isBing) {
        ret = ioctl(fd, OP_READ_MEM_Bing, &cm);
    } else {
        ret = syscall(SYS_ioctl, target, OP_READ_MEM, &cm);
    }

    // 检查驱动读取结果
    bool driver_success = (ret == 0);
    if (!driver_success) {
        // 修复：errno需要#include <errno.h>
        LOGE("驱动读取失败！返回值: %d, 错误: %s", ret, strerror(errno));
        // 尝试备用方法
        LOGI("尝试备用读取方法...");
        return read_proc_mem(pid, addr, buffer, size);
    }

    // 打印原始字节（验证驱动是否真的读取到数据）
    LOGI("驱动读取成功，原始字节（前8字节）:");
    uint8_t* bytes = (uint8_t*)buffer;
    // 修复：std::min需要#include <algorithm>
    for (int i = 0; i < std::min((size_t)8, size); i++) {
        LOGI("字节[%d]: 0x%02X", i, bytes[i]);
        // 检查是否还是初始的0xAA（驱动未写入数据）
        if (bytes[i] == 0xAA) {
            LOGE("警告：第%d字节未被驱动修改（可能读取无效）", i);
            // 自动切换到备用方法
            LOGI("自动切换到备用方法...");
            return read_proc_mem(pid, addr, buffer, size);
        }
    }

    return true;
}

bool driver::write(uintptr_t addr, void* buffer, size_t size) {
    if (!buffer || size == 0) {
        LOGE("写入失败：无效缓冲区或大小");
        return false;
    }

    COPY_MEMORY cm;
    cm.addr = addr;
    cm.buffer = buffer;
    cm.size = size;

    if (isBing) {
        cm.pid = pid;
        if (ioctl(fd, OP_WRITE_MEM_Bing, &cm) != 0) {
            LOGE("BING驱动写入失败（地址0x%lx）", addr);
            return false;
        }
    } else {
        if (syscall(SYS_ioctl, target, OP_WRITE_MEM, &cm) != 0) {
            LOGE("橘子驱动写入失败（地址0x%lx）", addr);
            return false;
        }
    }
    return true;
}

uintptr_t driver::get_module_base(char* name, pid_t pid) {
    if (!name || pid <= 0) {
        LOGE("获取模块基地址失败：无效参数");
        return 0;
    }

    MODULE_BASE mb;
    char buf[0x100];
    strncpy(buf, name, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    mb.pid = pid;
    mb.name = buf;
    mb.base = 0;

    if (isBing) {
        if (ioctl(fd, OP_MODULE_BASE_Bing, &mb) != 0) {
            LOGE("BING驱动获取模块基地址失败（%s）", name);
            return 0;
        }
    } else {
        if (syscall(SYS_ioctl, target, OP_MODULE_BASE, &mb) != 0) {
            LOGE("橘子驱动获取模块基地址失败（%s）", name);
            return 0;
        }
    }
    return mb.base;
}

bool driver::init_oxdriver() {
    handle = dlopen("/data/system/liborange.so", RTLD_LAZY);
    if (!handle) {
        LOGE("加载liborange.so失败：%s", dlerror());
        return false;
    }

    gettarget = (init)dlsym(handle, "_Z13initialkernelP21_copy_filedescription");
    if (!gettarget) {
        LOGE("获取gettarget失败：%s", dlerror());
        dlclose(handle);
        return false;
    }

    handle1 = dlopen("/data/system/liborangeinit.so", RTLD_LAZY);
    if (!handle1) {
        LOGE("加载liborangeinit.so失败：%s", dlerror());
        dlclose(handle);
        return false;
    }

    InitialTarget = (Init)dlsym(handle1, "_Z13initialkerneliiPc");
    if (!InitialTarget) {
        LOGE("获取InitialTarget失败：%s", dlerror());
        dlclose(handle1);
        dlclose(handle);
        return false;
    }
    return true;
}

bool driver::initialkernel(pid_t gamepid) {
    init_notification_manager();
    if (gamepid <= 0) {
        LOGE("无效目标PID：%d", gamepid);
        return false;
    }
    pid = gamepid;

    fd = open("/dev/BING", O_RDWR);
    if (fd != -1) {
        isBing = true;
        notificationManager->addMessage("使用BING驱动", 1);
        return true;
    }

    LOGE("未检测到BING驱动，尝试橘子驱动...");
    if (!init_oxdriver()) {
        return false;
    }

    while (target <= 0) {
        target = gettarget(&cf);
        if (target <= 0) usleep(100000);
    }

    char str_key[] = "shjKXCJTphack20071019=XCJ";
    InitialTarget(target, gamepid, str_key);

    dlclose(handle);
    system("su -c rm /data/system/liborange.so");
    dlclose(handle1);
    system("su -c rm /data/system/liborangeinit.so");

    notificationManager->addMessage("使用橘子驱动", 1);
    return true;
}
