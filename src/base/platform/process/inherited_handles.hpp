#pragma once

// CreateProcess 的 bInheritHandles=TRUE 交给子进程的是父进程此刻「所有」可继承句柄,
// 不只是 STARTUPINFO 里的三个标准流。MSVC 运行库打开的文件(fopen / std::ifstream)
// 默认可继承,而且不允许别人删除或改名:别的线程恰好正在读的文件,就这样被常驻
// 子进程一直占到它退出(升级备份安装目录里的文件时报错误 32)。
//
// 这个类把继承范围限定为显式列出的句柄(PROC_THREAD_ATTRIBUTE_HANDLE_LIST):
//
//   InheritedHandleList inherited({stdin_read, stdout_write, stderr_write});
//   if (!inherited.ready()) { /* 用 inherited.error_code() 报错 */ }
//   STARTUPINFOEXW si{};
//   si.StartupInfo.cb = sizeof(si);
//   si.lpAttributeList = inherited.attribute_list();
//   CreateProcessW(..., TRUE, flags | EXTENDED_STARTUPINFO_PRESENT, ...,
//                  &si.StartupInfo, &pi);
//
// 对象必须活到 CreateProcess 返回;列出的句柄本身要可继承。

#ifdef _WIN32

#include <initializer_list>
#include <vector>

struct _PROC_THREAD_ATTRIBUTE_LIST;

namespace acecode::platform {

class InheritedHandleList {
public:
    // 空句柄与 INVALID_HANDLE_VALUE 忽略;重复的只留一个(清单里有重复项时
    // CreateProcess 直接报参数错误,而 stderr 合流到 stdout 时两者就是同一个句柄)。
    explicit InheritedHandleList(std::initializer_list<void*> handles);
    ~InheritedHandleList();
    InheritedHandleList(const InheritedHandleList&) = delete;
    InheritedHandleList& operator=(const InheritedHandleList&) = delete;

    // 清单建好才为 true。一个可用句柄都没有也算没建好:那种情况调用方应当直接
    // 以 bInheritHandles=FALSE 启动。
    bool ready() const noexcept { return ready_; }
    // ready() 为 false 时的 Windows 错误码。
    unsigned long error_code() const noexcept { return error_code_; }
    // 填进 STARTUPINFOEX::lpAttributeList;ready() 为 false 时是 nullptr。
    _PROC_THREAD_ATTRIBUTE_LIST* attribute_list() noexcept;

private:
    std::vector<void*> handles_;
    std::vector<unsigned char> storage_;
    bool initialized_ = false;
    bool ready_ = false;
    unsigned long error_code_ = 0;
};

} // namespace acecode::platform

#endif
