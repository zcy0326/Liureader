#ifndef KERNELTOOLS_H
#define KERNELTOOLS_H

#include <cstdint>
#include <cstddef>
#include <sys/types.h>

class NotificationManager {
public:
    void addMessage(const char* msg, int type);
};

extern NotificationManager* notificationManager;

namespace driver {
    extern bool isBing;

    bool init_key(char* key);
    bool read(uintptr_t addr, void* buffer, size_t size);
    bool write(uintptr_t addr, void* buffer, size_t size);
    uintptr_t get_module_base(char* name, pid_t pid);

    // 模板化读写（支持任意类型）
    template <typename T>
    T read(uintptr_t addr) {
        T res;
        if (read(addr, &res, sizeof(T))) return res;
        return {};
    }

    template <typename T>
    bool write(uintptr_t addr, T value) {
        return write(addr, &value, sizeof(T));
    }

    bool init_oxdriver();
    bool initialkernel(pid_t gamepid);
}

#endif // KERNELTOOLS_H
