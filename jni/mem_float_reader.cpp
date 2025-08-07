#include "KernelTools.h"
#include <iostream>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <android/log.h>
#include <dirent.h>  // 目录操作头文件

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MemTool", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MemTool", __VA_ARGS__)

// 内存范围类型（映射Ca/Xa/Jh等场景）
enum MemoryRange {
    RANGE_ALL,       // 所有区域
    RANGE_CA,        // Ca: 代码段（r-x）
    RANGE_XA,        // Xa: 可执行区域（r-x，包含代码段和共享库）
    RANGE_JH,        // Jh: Java堆（rw-，无路径或[heap]）
    RANGE_DA,        // Da: 数据段（rw-）
    RANGE_ST         // St: 栈（rw-，[stack]）
};

// 去除字符串首尾空格
std::string trim(const std::string &s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start)) start++;
    auto end = s.end();
    do { end--; } while (std::distance(start, end) > 0 && std::isspace(*end));
    return std::string(start, end + 1);
}

// 解析内存范围（将用户输入映射为枚举）
MemoryRange parse_range(const std::string &input) {
    std::string range = trim(input);
    if (range == "0") return RANGE_ALL;
    if (range == "1" || range == "Ca" || range == "ca") return RANGE_CA;
    if (range == "2" || range == "Xa" || range == "xa") return RANGE_XA;
    if (range == "3" || range == "Jh" || range == "jh") return RANGE_JH;
    if (range == "4" || range == "Da" || range == "da") return RANGE_DA;
    if (range == "5" || range == "St" || range == "st") return RANGE_ST;
    return RANGE_ALL;  // 默认所有区域
}

// 检查地址是否属于指定内存范围
bool is_in_range(const std::string &line, MemoryRange range) {
    std::string perms = line.substr(line.find(' ') + 1, 3);  // 权限字符串（如r-x）
    std::string path = line.substr(line.find_last_of(' ') + 1);  // 路径（如[heap]）

    switch (range) {
        case RANGE_CA:  // 代码段（r-x，且路径为可执行文件）
            return (perms == "r-x" && (path.find(".so") == std::string::npos || path.find("/") != std::string::npos));
        case RANGE_XA:  // 可执行区域（r-x）
            return (perms == "r-x");
        case RANGE_JH:  // Java堆（rw-，无路径或[heap]）
            return (perms == "rw-" && (path.empty() || path == "[heap]"));
        case RANGE_DA:  // 数据段（rw-）
            return (perms == "rw-");
        case RANGE_ST:  // 栈（rw-，[stack]）
            return (perms == "rw-" && path.find("[stack]") != std::string::npos);
        default:  // 所有区域
            return true;
    }
}

// 检查地址有效性（含内存范围过滤）
bool is_address_valid(pid_t pid, uintptr_t addr, size_t size, MemoryRange range) {
    std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps_file(maps_path);
    if (!maps_file.is_open()) {
        LOGE("无法打开/proc/%d/maps（需root权限）", pid);
        return false;
    }

    std::string line;
    while (std::getline(maps_file, line)) {
        // 解析格式："start-end perm offset dev inode path"
        size_t dash_pos = line.find('-');
        if (dash_pos == std::string::npos) continue;

        uintptr_t start_addr = std::stoull(line.substr(0, dash_pos), nullptr, 16);
        size_t space_pos = line.find(' ', dash_pos);
        if (space_pos == std::string::npos) continue;

        uintptr_t end_addr = std::stoull(line.substr(dash_pos + 1, space_pos - dash_pos - 1), nullptr, 16);

        // 检查地址范围和内存范围
        if (addr >= start_addr && addr + size <= end_addr && 
            line[space_pos + 1] == 'r' &&  // 确保可读
            is_in_range(line, range)) {    // 检查是否属于目标范围
            maps_file.close();
            return true;
        }
    }

    maps_file.close();
    return false;
}

// 通过包名获取PID
pid_t get_pid_by_package(const std::string &package_name) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        LOGE("无法打开/proc目录（需root）");
        return -1;
    }

    dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        std::string pid_str = entry->d_name;
        if (pid_str.find_first_not_of("0123456789") != std::string::npos) continue;

        pid_t pid = std::stoi(pid_str);
        std::string cmdline_path = "/proc/" + pid_str + "/cmdline";
        std::ifstream cmdline_file(cmdline_path);
        if (!cmdline_file.is_open()) continue;

        std::string cmdline;
        std::getline(cmdline_file, cmdline, '\0');
        cmdline_file.close();

        if (trim(cmdline) == package_name) {
            closedir(dir);
            return pid;
        }
    }

    closedir(dir);
    return -1;
}

// 解析十六进制地址
uintptr_t parse_address(const std::string &addr_str) {
    std::string str = trim(addr_str);
    if (str.empty()) return 0;

    size_t pos = 0;
    if (str.substr(0, 2) == "0x") pos = 2;

    try {
        return std::stoull(str.substr(pos), nullptr, 16);
    } catch (...) {
        return 0;
    }
}

// 打印内存范围说明
void print_ranges() {
    std::cout << "\n===== 内存范围说明 =====" << std::endl;
    std::cout << "0: 所有区域（默认）" << std::endl;
    std::cout << "1 (Ca): 代码段（可执行+可读，r-x）" << std::endl;
    std::cout << "2 (Xa): 可执行区域（含代码段和共享库，r-x）" << std::endl;
    std::cout << "3 (Jh): Java堆（可读+可写，[heap]）" << std::endl;
    std::cout << "4 (Da): 数据段（可读+可写，rw-）" << std::endl;
    std::cout << "5 (St): 栈区域（可读+可写，[stack]）" << std::endl;
    std::cout << "======================" << std::endl;
}

// 打印使用说明
void print_usage() {
    std::cout << "\n===== 内存读写工具 =====" << std::endl;
    std::cout << "功能：读取指定进程的浮点/双精度值，支持内存范围过滤" << std::endl;
    std::cout << "退出：输入exit" << std::endl;
    print_ranges();
}

int main() {
    print_usage();

    // 获取目标PID（支持包名）
    pid_t target_pid = -1;
    std::string input;
    while (true) {
        std::cout << "\n选择输入方式（1=PID，2=包名）: ";
        if (!std::getline(std::cin, input)) {
            std::cerr << "输入错误，请重试" << std::endl;
            continue;
        }

        input = trim(input);
        if (input == "1") {  // 输入PID
            while (true) {
                std::cout << "输入目标进程PID: ";
                if (!std::getline(std::cin, input)) {
                    std::cerr << "输入错误，请重试" << std::endl;
                    continue;
                }
                input = trim(input);
                if (input.empty()) {
                    std::cout << "PID不能为空" << std::endl;
                    continue;
                }
                try {
                    target_pid = std::stoi(input);
                    if (target_pid > 0) break;
                    std::cout << "PID必须为正整数" << std::endl;
                } catch (...) {
                    std::cout << "无效PID" << std::endl;
                }
            }
            break;

        } else if (input == "2") {  // 输入包名
            std::cout << "输入目标包名（如com.example.game）: ";
            if (!std::getline(std::cin, input)) {
                std::cerr << "输入错误，请重试" << std::endl;
                continue;
            }
            std::string pkg = trim(input);
            if (pkg.empty()) {
                std::cout << "包名不能为空" << std::endl;
                continue;
            }
            target_pid = get_pid_by_package(pkg);
            if (target_pid <= 0) {
                std::cout << "未找到包名为[" << pkg << "]的进程" << std::endl;
                continue;
            }
            std::cout << "找到对应PID: " << target_pid << std::endl;
            break;

        } else if (input == "exit") {
            std::cout << "程序退出" << std::endl;
            return 0;

        } else {
            std::cout << "请输入1或2" << std::endl;
        }
    }

    // 选择内存范围
    MemoryRange range;
    while (true) {
        std::cout << "\n选择内存范围（输入编号或简写，如1或Ca）: ";
        if (!std::getline(std::cin, input)) {
            std::cerr << "输入错误，请重试" << std::endl;
            continue;
        }
        input = trim(input);
        if (input == "exit") {
            std::cout << "程序退出" << std::endl;
            return 0;
        }
        range = parse_range(input);
        if (range >= RANGE_ALL && range <= RANGE_ST) break;
        std::cout << "无效范围，请参考说明重新输入" << std::endl;
    }

    // 初始化内核工具
    std::cout << "初始化内核工具（PID: " << target_pid << "）..." << std::endl;
    if (!driver::initialkernel(target_pid)) {
        std::cerr << "初始化失败，请检查驱动" << std::endl;
        return 1;
    }

    // 循环读取内存
    while (true) {
        std::cout << "\n输入内存地址（如0x4021B810，输入exit退出）: ";
        if (!std::getline(std::cin, input)) {
            std::cerr << "输入错误，请重试" << std::endl;
            continue;
        }
        input = trim(input);
        if (input == "exit") {
            std::cout << "程序退出" << std::endl;
            break;
        }

        uintptr_t addr = parse_address(input);
        if (addr == 0) {
            std::cout << "无效地址格式（需十六进制）" << std::endl;
            continue;
        }

        // 选择数据类型
        std::cout << "选择数据类型（1=float(4字节)，2=double(8字节)）: ";
        if (!std::getline(std::cin, input)) {
            std::cerr << "输入错误，请重试" << std::endl;
            continue;
        }
        input = trim(input);
        if (input != "1" && input != "2") {
            std::cout << "请输入1或2" << std::endl;
            continue;
        }

        // 检查地址有效性（含范围过滤）
        size_t data_size = (input == "1") ? 4 : 8;
        if (!is_address_valid(target_pid, addr, data_size, range)) {
            std::cout << "地址0x" << std::hex << addr << std::dec 
                      << " 不在指定内存范围或不可读（需root权限）" << std::endl;
            continue;
        }

        // 读取并输出数据
        if (input == "1") {  // float
            float value = driver::read<float>(addr);
            std::cout << "地址0x" << std::hex << addr << std::dec 
                      << " 的float值: " << value << std::endl;
        } else {  // double
            double value = driver::read<double>(addr);
            std::cout << "地址0x" << std::hex << addr << std::dec 
                      << " 的double值: " << value << std::endl;
        }
    }

    return 0;
}
