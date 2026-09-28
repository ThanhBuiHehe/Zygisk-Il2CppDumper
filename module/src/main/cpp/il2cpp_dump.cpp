//
// Created by Perfare on 2020/7/4.
//

#include "il2cpp_dump.h"
#include <cstdio>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <link.h>
#include <csignal>
#include <csetjmp>
#include <sys/stat.h>
#include <elf.h>
#include <algorithm>
#include "xdl.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#ifndef SYS_process_vm_readv
#if defined(__NR_process_vm_readv)
#define SYS_process_vm_readv __NR_process_vm_readv
#endif
#endif

#ifndef SYS_pread64
#if defined(__NR_pread64)
#define SYS_pread64 __NR_pread64
#endif
#endif

#define DO_API(r, n, p) r (*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

static uint64_t il2cpp_base = 0;

// Signal handling state for safe memory read
static sigjmp_buf g_segv_jmp_buf;
static volatile sig_atomic_t g_in_safe_read = 0;

static void segv_signal_handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    (void)context;
    if (g_in_safe_read) {
        siglongjmp(g_segv_jmp_buf, 1);
    }
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

class ScopedSignalHandler {
public:
    ScopedSignalHandler() {
        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO;
        sa.sa_sigaction = segv_signal_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old_segv_);
        sigaction(SIGBUS, &sa, &old_bus_);
    }

    ~ScopedSignalHandler() {
        sigaction(SIGSEGV, &old_segv_, nullptr);
        sigaction(SIGBUS, &old_bus_, nullptr);
    }

private:
    struct sigaction old_segv_{};
    struct sigaction old_bus_{};
};

static bool safe_mem_read(void *dst, const void *src, size_t len, int mem_fd) {
    if (!dst || !src || len == 0) return false;

#if defined(SYS_process_vm_readv)
    struct iovec local_iov = { dst, len };
    struct iovec remote_iov = { const_cast<void*>(src), len };
    ssize_t rc = syscall(SYS_process_vm_readv, getpid(), &local_iov, 1, &remote_iov, 1, 0);
    if (rc == static_cast<ssize_t>(len)) {
        return true;
    }
#endif
    if (mem_fd >= 0) {
#if defined(SYS_pread64)
        ssize_t prc = syscall(SYS_pread64, mem_fd, dst, len, static_cast<off64_t>(reinterpret_cast<uintptr_t>(src)));
        if (prc == static_cast<ssize_t>(len)) {
            return true;
        }
#endif
    }

    
    g_in_safe_read = 1;
    if (sigsetjmp(g_segv_jmp_buf, 1) == 0) {
        memcpy(dst, src, len);
        g_in_safe_read = 0;
        return true;
    } else {
        g_in_safe_read = 0;
        return false;
    }
}

static bool ensure_directory(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return false;
}

static bool is_array_or_generic(const Il2CppType *type) {
    if (!type) return true;
    return (type->type == IL2CPP_TYPE_SZARRAY ||
            type->type == IL2CPP_TYPE_ARRAY ||
            type->type == IL2CPP_TYPE_GENERICINST ||
            type->type == IL2CPP_TYPE_VAR ||
            type->type == IL2CPP_TYPE_MVAR);
}

static Il2CppClass *safe_il2cpp_class_from_type(const Il2CppType *type) {
    if (!type || !il2cpp_class_from_type) return nullptr;
    if (is_array_or_generic(type)) return nullptr;
    return il2cpp_class_from_type(type);
}

static std::string safe_get_type_name(const Il2CppType *type) {
    if (!type) return "void";
    if (type->type == IL2CPP_TYPE_SZARRAY) {
        if (type->data.type) {
            return safe_get_type_name(type->data.type) + "[]";
        }
        return "object[]";
    }
    if (type->type == IL2CPP_TYPE_ARRAY) {
        return "System.Array";
    }
    if (type->type == IL2CPP_TYPE_GENERICINST || type->type == IL2CPP_TYPE_VAR || type->type == IL2CPP_TYPE_MVAR) {
        return "T";
    }
    auto klass = safe_il2cpp_class_from_type(type);
    if (klass && il2cpp_class_get_name) {
        const char *name = il2cpp_class_get_name(klass);
        if (name) return name;
    }
    return "object";
}

void init_il2cpp_api(void *handle) {
#define DO_API(r, n, p) {                      \
    n = (r (*) p)xdl_sym(handle, #n, nullptr); \
    if(!n) {                                   \
        LOGW("api not found %s", #n);          \
    }                                          \
}

#include "il2cpp-api-functions.h"

#undef DO_API
}

std::string get_method_modifier(uint32_t flags) {
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:
            outPut << "private ";
            break;
        case METHOD_ATTRIBUTE_PUBLIC:
            outPut << "public ";
            break;
        case METHOD_ATTRIBUTE_FAMILY:
            outPut << "protected ";
            break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:
            outPut << "internal ";
            break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) {
        outPut << "static ";
    }
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "sealed override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT) {
            outPut << "virtual ";
        } else {
            outPut << "override ";
        }
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        outPut << "extern ";
    }
    return outPut.str();
}

bool _il2cpp_type_is_byref(const Il2CppType *type) {
    auto byref = type->byref;
    if (il2cpp_type_is_byref) {
        byref = il2cpp_type_is_byref(type);
    }
    return byref;
}

std::string dump_method(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
    
        if (method->methodPointer) {
            outPut << "\t// RVA: 0x";
            outPut << std::hex << (uint64_t) method->methodPointer - il2cpp_base;
            outPut << " VA: 0x";
            outPut << std::hex << (uint64_t) method->methodPointer;
        } else {
            outPut << "\t// RVA: 0x VA: 0x0";
        }
        /*if (method->slot != 65535) {
            outPut << " Slot: " << std::dec << method->slot;
        }*/
        outPut << "\n\t";
        uint32_t iflags = 0;
        auto flags = il2cpp_method_get_flags(method, &iflags);
        outPut << get_method_modifier(flags);
        //TODO genericContainerIndex
        auto return_type = il2cpp_method_get_return_type(method);
        if (_il2cpp_type_is_byref(return_type)) {
            outPut << "ref ";
        }
        outPut << safe_get_type_name(return_type) << " " << il2cpp_method_get_name(method)
               << "(";
        auto param_count = il2cpp_method_get_param_count(method);
        for (int i = 0; i < param_count; ++i) {
            auto param = il2cpp_method_get_param(method, i);
            auto attrs = param->attrs;
            if (_il2cpp_type_is_byref(param)) {
                if (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN)) {
                    outPut << "out ";
                } else if (attrs & PARAM_ATTRIBUTE_IN && !(attrs & PARAM_ATTRIBUTE_OUT)) {
                    outPut << "in ";
                } else {
                    outPut << "ref ";
                }
            } else {
                if (attrs & PARAM_ATTRIBUTE_IN) {
                    outPut << "[In] ";
                }
                if (attrs & PARAM_ATTRIBUTE_OUT) {
                    outPut << "[Out] ";
                }
            }
            outPut << safe_get_type_name(param) << " "
                   << il2cpp_method_get_param_name(method, i);
            outPut << ", ";
        }
        if (param_count > 0) {
            outPut.seekp(-2, outPut.cur);
        }
        outPut << ") { }\n";
        //TODO GenericInstMethod
    }
    return outPut.str();
}

std::string dump_property(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        //TODO attribute
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        outPut << "\t";
        Il2CppClass *prop_class = nullptr;
        const Il2CppType *prop_type = nullptr;
        uint32_t iflags = 0;
        if (get) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            prop_type = il2cpp_method_get_return_type(get);
            prop_class = safe_il2cpp_class_from_type(prop_type);
        } else if (set) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            prop_type = il2cpp_method_get_param(set, 0);
            prop_class = safe_il2cpp_class_from_type(prop_type);
        }
        if (prop_class && il2cpp_class_get_name) {
            outPut << il2cpp_class_get_name(prop_class) << " " << prop_name << " { ";
            if (get) {
                outPut << "get; ";
            }
            if (set) {
                outPut << "set; ";
            }
            outPut << "}\n";
        } else if (prop_type) {
            outPut << safe_get_type_name(prop_type) << " " << prop_name << " { ";
            if (get) {
                outPut << "get; ";
            }
            if (set) {
                outPut << "set; ";
            }
            outPut << "}\n";
        } else {
            if (prop_name) {
                outPut << " // unknown property " << prop_name << "\n";
            }
        }
    }
    return outPut.str();
}
std::string get_method_signature(const MethodInfo *method, Il2CppClass *klass) {
    std::stringstream sig;
    
    
    auto return_type = method ? il2cpp_method_get_return_type(method) : nullptr;
    if (return_type) {
        if (!is_array_or_generic(return_type)) {
            auto return_class = safe_il2cpp_class_from_type(return_type);
            if (return_class) {
                const char* ns = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(return_class) : nullptr;
                const char* name = il2cpp_class_get_name ? il2cpp_class_get_name(return_class) : nullptr;
                if (ns && name && strlen(ns) > 0) sig << ns << "." << name << " ";
                else if (name) sig << name << " ";
            } else {
                sig << safe_get_type_name(return_type) << " ";
            }
        } else {
            sig << safe_get_type_name(return_type) << " ";
        }
    }
    
    
    const char* kNs = (klass && il2cpp_class_get_namespace) ? il2cpp_class_get_namespace(klass) : nullptr;
    const char* kName = (klass && il2cpp_class_get_name) ? il2cpp_class_get_name(klass) : nullptr;
    const char* mName = (method && il2cpp_method_get_name) ? il2cpp_method_get_name(method) : nullptr;
    
    if (kNs && kName && strlen(kNs) > 0) sig << kNs << "." << kName << "::";
    else if (kName) sig << kName << "::";
    
    if (mName) sig << mName << "(";
    else sig << "unknown(";
    
    
    auto param_count = method ? il2cpp_method_get_param_count(method) : 0;
    for (int i = 0; i < param_count; ++i) {
        auto param = il2cpp_method_get_param(method, i);
        if (param) {
            if (!is_array_or_generic(param)) {
                auto param_class = safe_il2cpp_class_from_type(param);
                if (param_class) {
                    const char* pNs = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(param_class) : nullptr;
                    const char* pName = il2cpp_class_get_name ? il2cpp_class_get_name(param_class) : nullptr;
                    if (pNs && pName && strlen(pNs) > 0) sig << pNs << "." << pName;
                    else if (pName) sig << pName;
                } else {
                    sig << safe_get_type_name(param);
                }
            } else {
                sig << safe_get_type_name(param);
            }
        }
        if (i < param_count - 1) sig << ", ";
    }
    sig << ")";
    
    return sig.str();
}
std::string dump_field(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Fields\n";
    auto is_enum = il2cpp_class_is_enum(klass);
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        //TODO attribute
        outPut << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:
                outPut << "private ";
                break;
            case FIELD_ATTRIBUTE_PUBLIC:
                outPut << "public ";
                break;
            case FIELD_ATTRIBUTE_FAMILY:
                outPut << "protected ";
                break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:
                outPut << "internal ";
                break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:
                outPut << "protected internal ";
                break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            outPut << "const ";
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) {
                outPut << "static ";
            }
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) {
                outPut << "readonly ";
            }
        }
        auto field_type = il2cpp_field_get_type(field);
        outPut << safe_get_type_name(field_type) << " " << (il2cpp_field_get_name ? il2cpp_field_get_name(field) : "unknown");
        //TODO 获取构造函数初始化后的字段值
        if (attrs & FIELD_ATTRIBUTE_LITERAL && is_enum) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        outPut << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return outPut.str();
}

std::string dump_type(const Il2CppType *type) {
    std::stringstream outPut;
    auto *klass = safe_il2cpp_class_from_type(type);
    if (!klass) return "";
    const char *kNs = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(klass) : "";
    outPut << "\n// Namespace: " << (kNs ? kNs : "") << "\n";
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) {
        outPut << "[Serializable]\n";
    }
    //TODO attribute
    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum = il2cpp_class_is_enum(klass);
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:
            outPut << "public ";
            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:
            outPut << "internal ";
            break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:
            outPut << "private ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:
            outPut << "protected ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "static ";
    } else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
    } else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "sealed ";
    }
    if (flags & TYPE_ATTRIBUTE_INTERFACE) {
        outPut << "interface ";
    } else if (is_enum) {
        outPut << "enum ";
    } else if (is_valuetype) {
        outPut << "struct ";
    } else {
        outPut << "class ";
    }
    outPut << il2cpp_class_get_name(klass); //TODO genericContainerIndex
    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!is_valuetype && !is_enum && parent) {
        auto parent_type = il2cpp_class_get_type(parent);
        if (parent_type->type != IL2CPP_TYPE_OBJECT) {
            extends.emplace_back(il2cpp_class_get_name(parent));
        }
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter)) {
        extends.emplace_back(il2cpp_class_get_name(itf));
    }
    if (!extends.empty()) {
        outPut << " : " << extends[0];
        for (int i = 1; i < extends.size(); ++i) {
            outPut << ", " << extends[i];
        }
    }
    outPut << "\n{";
    outPut << dump_field(klass);
    outPut << dump_property(klass);
    outPut << dump_method(klass);
    //TODO EventInfo
    outPut << "}\n";
    return outPut.str();
}

void il2cpp_api_init(void *handle) {
    LOGI("il2cpp_handle: %p", handle);
    sleep(30);
    init_il2cpp_api(handle);
    if (il2cpp_domain_get_assemblies) {
        Dl_info dlInfo;
        if (dladdr((void *) il2cpp_domain_get_assemblies, &dlInfo)) {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
        LOGI("il2cpp_base: %" PRIx64"", il2cpp_base);
    } else {
        LOGE("Failed to initialize il2cpp api.");
        return;
    }
    while (!il2cpp_is_vm_thread(nullptr)) {
        LOGI("Waiting for il2cpp_init...");
        sleep(1);
    }
    auto domain = il2cpp_domain_get();
    il2cpp_thread_attach(domain);
}
void dump_script_json(const char *outDir) {
    LOGI("Generating script.json");
    
    std::string jsonPath = std::string(outDir).append("/files/script.json");
    std::ofstream jsonStream(jsonPath);
    
    if (!jsonStream.is_open()) {
        LOGE("Fail to open script.json");
        return;
    }
    
    jsonStream << "{\n  \"ScriptMethod\": [\n";
    
    size_t size = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    
    bool first = true;
    int methodCount = 0;
    
    for (int i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        
        const char* imageName = il2cpp_image_get_name(image);
        LOGI("Processing image %d: %s", i, imageName ? imageName : "NULL");
        
        auto classCount = il2cpp_image_get_class_count(image);
        for (int j = 0; j < classCount; ++j) {
            auto klass = il2cpp_image_get_class(image, j);
            if (!klass) continue;
            
            void *iter = nullptr;
            const MethodInfo *method = nullptr;
            
            while ((method = il2cpp_class_get_methods(const_cast<Il2CppClass*>(klass), &iter))) {
                if (!method->methodPointer) continue;
                
                uint64_t rva = (uint64_t)method->methodPointer - il2cpp_base;
                if (rva == 0) continue;
                
                std::string className = il2cpp_class_get_name(const_cast<Il2CppClass*>(klass));
                std::string namespaceName = il2cpp_class_get_namespace(const_cast<Il2CppClass*>(klass));
                std::string methodName = il2cpp_method_get_name(method);
                
                std::string fullName = namespaceName.empty() ? 
                    className + "$$" + methodName :
                    namespaceName + "." + className + "$$" + methodName;
                
                std::string signature = get_method_signature(method, const_cast<Il2CppClass*>(klass));
                
                if (!first) jsonStream << ",\n";
                jsonStream << "    {\"Address\": " << rva 
                           << ", \"Name\": \"" << fullName 
                           << "\", \"Signature\": \"" << signature << "\"}";
                first = false;
                methodCount++;
                
                // Flush định kỳ
                if (methodCount % 5000 == 0) {
                    jsonStream.flush();
                    LOGI("  Processed %d methods", methodCount);
                }
            }
        }
    }
    
    jsonStream << "\n  ],\n";
    jsonStream << "  \"ScriptString\": [],\n";
    jsonStream << "  \"ScriptMetadata\": {\"DumpVersion\": 6}\n";
    jsonStream << "}\n";
    
    jsonStream.flush();
    jsonStream.close();
    
    LOGI("script.json created with %d methods", methodCount);
}
static uintptr_t s_metadata_addr = 0;
static size_t s_metadata_size = 0;
static size_t s_lib_size = 0;

struct MemoryMapRange {
    uintptr_t start;
    uintptr_t end;
    char perms[5];
    uint64_t offset;
    std::string path;
};

static inline bool is_metadata_magic(const void *ptr) {
    if (!ptr) return false;
    const auto *bytes = reinterpret_cast<const uint8_t*>(ptr);
    return bytes[0] == 0xAF && bytes[1] == 0x1B && bytes[2] == 0xB1 && bytes[3] == 0xFA;
}

void dump_metadata(const char *outDir) {
    if (!outDir) {
        LOGE("dump_metadata: outDir is null");
        return;
    }

    LOGI("Starting dump_metadata...");
    ScopedSignalHandler sig_guard;

    std::string filesDir = std::string(outDir) + "/files";
    if (!ensure_directory(filesDir)) {
        LOGE("Failed to ensure directory: %s", filesDir.c_str());
        return;
    }

    int mem_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (mem_fd < 0) {
        LOGW("Failed to open /proc/self/mem via syscall open, fallback to direct memory reads");
    }

    
    int maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps_fd < 0) {
        LOGE("Failed to open /proc/self/maps");
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    std::string maps_content;
    char read_buf[8192];
    ssize_t n;
    while ((n = read(maps_fd, read_buf, sizeof(read_buf))) > 0) {
        maps_content.append(read_buf, n);
    }
    close(maps_fd);

    std::vector<MemoryMapRange> maps;
    std::istringstream stream(maps_content);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        MemoryMapRange range{};
        char perms[5] = {0};
        char path_buf[512] = {0};
        uintptr_t start = 0, end = 0;
        uint64_t offset = 0;

        int fields = sscanf(line.c_str(), "%" PRIxPTR "-%" PRIxPTR " %4s %" PRIx64 " %*s %*s %511s",
                            &start, &end, perms, &offset, path_buf);
        if (fields >= 3) {
            range.start = start;
            range.end = end;
            strncpy(range.perms, perms, sizeof(range.perms) - 1);
            range.offset = offset;
            if (fields >= 5) {
                range.path = path_buf;
            }
            maps.push_back(range);
        }
    }

    uintptr_t metadata_addr = 0;
    uintptr_t metadata_map_end = 0;

    
    for (const auto &m : maps) {
        if (m.perms[0] == 'r' && m.path.find("global-metadata.dat") != std::string::npos) {
            uint8_t head[8] = {0};
            if (safe_mem_read(head, reinterpret_cast<const void*>(m.start), sizeof(head), mem_fd)) {
                if (is_metadata_magic(head)) {
                    int32_t ver = *reinterpret_cast<const int32_t*>(head + 4);
                    if (ver >= 20 && ver <= 40) {
                        metadata_addr = m.start;
                        metadata_map_end = m.end;
                        LOGI("Found metadata by path match at %p (version: %d)", (void*)metadata_addr, ver);
                        break;
                    }
                }
            }
        }
    }

    
    if (metadata_addr == 0) {
        constexpr size_t SCAN_CHUNK_SIZE = 1024 * 1024; // 1 MB
        std::vector<uint8_t> scan_buf(SCAN_CHUNK_SIZE);

        for (const auto &m : maps) {
            
            if (m.perms[0] != 'r' || m.perms[2] == 'x') continue;
            if (m.end <= m.start || (m.end - m.start) < 256 * 1024) continue;
            if (m.path.find(".so") != std::string::npos) continue;
            if (m.path.find("/system/") != std::string::npos ||
                m.path.find("/apex/") != std::string::npos ||
                m.path.find("/vendor/") != std::string::npos ||
                m.path.find("/dev/") != std::string::npos ||
                m.path.find("[stack") != std::string::npos) {
                continue;
            }

            
            uint8_t head[8] = {0};
            if (safe_mem_read(head, reinterpret_cast<const void*>(m.start), sizeof(head), mem_fd)) {
                if (is_metadata_magic(head)) {
                    int32_t ver = *reinterpret_cast<const int32_t*>(head + 4);
                    if (ver >= 20 && ver <= 40) {
                        metadata_addr = m.start;
                        metadata_map_end = m.end;
                        LOGI("Found metadata at range start %p (version: %d, path: %s)",
                             (void*)metadata_addr, ver, m.path.c_str());
                        break;
                    }
                }
            }

            
            for (uintptr_t cur = m.start; cur < m.end; cur += (SCAN_CHUNK_SIZE - 16)) {
                size_t cur_len = std::min(SCAN_CHUNK_SIZE, m.end - cur);
                if (!safe_mem_read(scan_buf.data(), reinterpret_cast<const void*>(cur), cur_len, mem_fd)) {
                    continue;
                }
                for (size_t off = 0; off + 8 <= cur_len; off += 4) {
                    if (is_metadata_magic(&scan_buf[off])) {
                        int32_t ver = *reinterpret_cast<const int32_t*>(&scan_buf[off + 4]);
                        if (ver >= 20 && ver <= 40) {
                            metadata_addr = cur + off;
                            metadata_map_end = m.end;
                            LOGI("Found metadata in memory scan at %p (version: %d, map: %s)",
                                 (void*)metadata_addr, ver, m.path.c_str());
                            break;
                        }
                    }
                }
                if (metadata_addr != 0) break;
            }
            if (metadata_addr != 0) break;
        }
    }

    if (metadata_addr == 0) {
        LOGE("Failed to find global-metadata.dat in process memory!");
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    
    size_t metadata_size = 0;
    int32_t metadata_version = 0;
    uint8_t header_buf[1024] = {0};
    if (safe_mem_read(header_buf, reinterpret_cast<const void*>(metadata_addr), sizeof(header_buf), mem_fd)) {
        metadata_version = *reinterpret_cast<const int32_t*>(header_buf + 4);
        const uint32_t *pairs = reinterpret_cast<const uint32_t*>(header_buf + 8);
        uint64_t max_end = 0;
        for (int i = 0; i < 32; ++i) {
            uint32_t sec_off = pairs[i * 2];
            uint32_t sec_sz = pairs[i * 2 + 1];
            if (sec_off > 0 && sec_sz > 0 && sec_off < 100 * 1024 * 1024 && sec_sz < 100 * 1024 * 1024) {
                uint64_t end_pos = static_cast<uint64_t>(sec_off) + sec_sz;
                if (end_pos > max_end && end_pos < 100 * 1024 * 1024) {
                    max_end = end_pos;
                }
            }
        }
        if (max_end > 0) {
            metadata_size = static_cast<size_t>(max_end);
        }
    }

    if (metadata_size == 0) {
        metadata_size = std::min<size_t>(metadata_map_end - metadata_addr, 60 * 1024 * 1024);
        LOGW("Could not calculate exact size from header, falling back to: %zu bytes", metadata_size);
    } else {
        LOGI("Metadata header parsed: version %d, calculated size: %zu bytes (%.2f MB)",
             metadata_version, metadata_size, metadata_size / (1024.0 * 1024.0));
    }

    s_metadata_addr = metadata_addr;
    s_metadata_size = metadata_size;

    
    std::string out_path = filesDir + "/global-metadata.dat";
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        LOGE("Failed to open %s for writing", out_path.c_str());
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    constexpr size_t WRITE_CHUNK_SIZE = 1024 * 1024; // 1 MB
    std::vector<uint8_t> write_buf(WRITE_CHUNK_SIZE);
    size_t total_written = 0;

    while (total_written < metadata_size) {
        size_t cur_len = std::min(WRITE_CHUNK_SIZE, metadata_size - total_written);
        uintptr_t cur_src = metadata_addr + total_written;

        if (!safe_mem_read(write_buf.data(), reinterpret_cast<const void*>(cur_src), cur_len, mem_fd)) {
            LOGE("Failed reading metadata memory at %p (offset %zu)", (void*)cur_src, total_written);
            break;
        }

        ssize_t w = write(out_fd, write_buf.data(), cur_len);
        if (w != static_cast<ssize_t>(cur_len)) {
            LOGE("Failed writing metadata to file at offset %zu", total_written);
            break;
        }

        total_written += cur_len;

        
        if (total_written % (10 * 1024 * 1024) == 0 || total_written == metadata_size) {
            fsync(out_fd);
            LOGI("Metadata dump progress: %zu / %zu bytes (%.1f%%)",
                 total_written, metadata_size, (total_written * 100.0) / metadata_size);
        }
    }

    fsync(out_fd);
    close(out_fd);
    if (mem_fd >= 0) close(mem_fd);

    LOGI("dump_metadata completed: %s (%zu bytes written)", out_path.c_str(), total_written);
}

struct DlIterateContext {
    uintptr_t target_base = 0;
    uintptr_t found_base = 0;
    std::string lib_path;
    std::vector<ElfW(Phdr)> phdrs;
    bool found = false;
};

static int dl_iterate_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    auto *ctx = reinterpret_cast<DlIterateContext*>(data);
    if (!info) return 0;

    bool match = false;
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so") != nullptr) {
        match = true;
    } else if (ctx->target_base != 0 && static_cast<uintptr_t>(info->dlpi_addr) == ctx->target_base) {
        match = true;
    }

    if (match) {
        ctx->found = true;
        ctx->found_base = static_cast<uintptr_t>(info->dlpi_addr);
        if (info->dlpi_name) {
            ctx->lib_path = info->dlpi_name;
        }
        if (info->dlpi_phdr && info->dlpi_phnum > 0) {
            ctx->phdrs.assign(info->dlpi_phdr, info->dlpi_phdr + info->dlpi_phnum);
        }
        return 1;
    }
    return 0;
}

void dump_libil2cpp(const char *outDir) {
    if (!outDir) {
        LOGE("dump_libil2cpp: outDir is null");
        return;
    }

    LOGI("Start dump_libil2cpp");
    ScopedSignalHandler sig_guard;

    std::string filesDir = std::string(outDir) + "/files";
    if (!ensure_directory(filesDir)) {
        LOGE("Failed to ensure directory: %s", filesDir.c_str());
        return;
    }
    DlIterateContext dl_ctx;
    dl_ctx.target_base = static_cast<uintptr_t>(il2cpp_base);
    dl_iterate_phdr(dl_iterate_cb, &dl_ctx);

    uintptr_t base = 0;
    if (dl_ctx.found && dl_ctx.found_base != 0) {
        base = dl_ctx.found_base;
    } else if (il2cpp_base != 0) {
        base = static_cast<uintptr_t>(il2cpp_base);
    }

    if (base == 0) {
        LOGE("dump_libil2cpp: failed to determine libil2cpp base address!");
        return;
    }
    LOGI("Target libil2cpp base: %p (path: %s)", reinterpret_cast<void*>(base), dl_ctx.lib_path.c_str());

    int mem_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (mem_fd < 0) {
        LOGW("Failed to open /proc/self/mem via syscall open, fallback to direct memory reads");
    }
    ElfW(Ehdr) ehdr{};
    if (!safe_mem_read(&ehdr, reinterpret_cast<const void*>(base), sizeof(ehdr), mem_fd)) {
        LOGE("Failed to read ELF header at %p", reinterpret_cast<void*>(base));
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        LOGE("Invalid ELF magic at %p", reinterpret_cast<void*>(base));
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    LOGI("ELF Header verified: class %d, machine 0x%x, phoff 0x%llx, phnum %d",
         ehdr.e_ident[EI_CLASS], ehdr.e_machine, static_cast<unsigned long long>(ehdr.e_phoff), ehdr.e_phnum);

    
    std::vector<ElfW(Phdr)> phdrs;
    if (!dl_ctx.phdrs.empty()) {
        phdrs = dl_ctx.phdrs;
    } else if (ehdr.e_phnum > 0 && ehdr.e_phoff > 0) {
        phdrs.resize(ehdr.e_phnum);
        if (!safe_mem_read(phdrs.data(), reinterpret_cast<const void*>(base + ehdr.e_phoff),
                          ehdr.e_phnum * sizeof(ElfW(Phdr)), mem_fd)) {
            LOGE("Failed to read ELF program headers from %p", reinterpret_cast<void*>(base + ehdr.e_phoff));
            if (mem_fd >= 0) close(mem_fd);
            return;
        }
    }

    
    uintptr_t max_vaddr = 0;
    int pt_load_count = 0;
    for (const auto &p : phdrs) {
        if (p.p_type == PT_LOAD) {
            pt_load_count++;
            uintptr_t seg_end = p.p_vaddr + p.p_memsz;
            if (seg_end > max_vaddr) {
                max_vaddr = seg_end;
            }
        }
    }

    if (max_vaddr == 0 || pt_load_count == 0) {
        LOGE("No valid PT_LOAD segments found in libil2cpp!");
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    size_t lib_total_size = static_cast<size_t>(max_vaddr);
    s_lib_size = lib_total_size;
    LOGI("libil2cpp size from %d PT_LOAD segments: %zu bytes (%.2f MB)",
         pt_load_count, lib_total_size, lib_total_size / (1024.0 * 1024.0));

    
    std::string out_path = filesDir + "/libil2cpp.so";
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        LOGE("Failed to open %s for writing", out_path.c_str());
        if (mem_fd >= 0) close(mem_fd);
        return;
    }

    
    ElfW(Ehdr) patched_ehdr = ehdr;
    if (patched_ehdr.e_shoff >= lib_total_size) {
        patched_ehdr.e_shoff = 0;
        patched_ehdr.e_shnum = 0;
        patched_ehdr.e_shstrndx = 0;
    }

    std::vector<ElfW(Phdr)> patched_phdrs = phdrs;
    for (auto &p : patched_phdrs) {
        if (p.p_type == PT_LOAD) {
            p.p_offset = p.p_vaddr;
            p.p_filesz = p.p_memsz;
        }
    }

    
    constexpr size_t DUMP_CHUNK_SIZE = 1024 * 1024; // 1 MB
    std::vector<uint8_t> chunk_buf(DUMP_CHUNK_SIZE);
    size_t total_written = 0;

    for (const auto &p : phdrs) {
        if (p.p_type != PT_LOAD) continue;

        uintptr_t seg_vaddr = p.p_vaddr;
        size_t seg_memsz = p.p_memsz;
        uintptr_t seg_src = base + seg_vaddr;

        if (lseek64(out_fd, static_cast<off64_t>(seg_vaddr), SEEK_SET) == -1) {
            LOGE("Failed to lseek to segment vaddr 0x%llx", static_cast<unsigned long long>(seg_vaddr));
            continue;
        }

        size_t seg_written = 0;
        while (seg_written < seg_memsz) {
            size_t cur_len = std::min(DUMP_CHUNK_SIZE, seg_memsz - seg_written);
            uintptr_t cur_src = seg_src + seg_written;

            if (!safe_mem_read(chunk_buf.data(), reinterpret_cast<const void*>(cur_src), cur_len, mem_fd)) {
                
                memset(chunk_buf.data(), 0, cur_len);
            }

            
            if (seg_vaddr == 0 && seg_written == 0) {
                if (sizeof(patched_ehdr) <= cur_len) {
                    memcpy(chunk_buf.data(), &patched_ehdr, sizeof(patched_ehdr));
                }
                size_t phdrs_off = patched_ehdr.e_phoff;
                size_t phdrs_bytes = patched_phdrs.size() * sizeof(ElfW(Phdr));
                if (phdrs_off + phdrs_bytes <= cur_len) {
                    memcpy(chunk_buf.data() + phdrs_off, patched_phdrs.data(), phdrs_bytes);
                }
            }

            ssize_t w = write(out_fd, chunk_buf.data(), cur_len);
            if (w != static_cast<ssize_t>(cur_len)) {
                LOGE("Failed writing libil2cpp at vaddr 0x%llx", static_cast<unsigned long long>(seg_vaddr + seg_written));
                break;
            }

            seg_written += cur_len;
            total_written += cur_len;

            
            if (total_written % (10 * 1024 * 1024) == 0) {
                fsync(out_fd);
                LOGI("libil2cpp dump progress: %zu bytes written", total_written);
            }
        }
    }

    fsync(out_fd);
    close(out_fd);
    if (mem_fd >= 0) close(mem_fd);

    LOGI("dump_libil2cpp completed: %s (%zu bytes written)", out_path.c_str(), total_written);
}

void save_dump_info(const char *outDir) {
    if (!outDir) return;
    auto infoPath = std::string(outDir).append("/files/dump_info.txt");
    std::ofstream out(infoPath);
    if (out.is_open()) {
        out << "====================================================\n";
        out << "              ZYGISK-IL2CPPDUMPER INFO              \n";
        out << "====================================================\n";
        out << "Dump Address (il2cpp_base): 0x" << std::hex << il2cpp_base << "\n";
        out << "Metadata Address:           0x" << std::hex << s_metadata_addr << "\n";
        out << "Metadata Size:              0x" << std::hex << s_metadata_size << " (" << std::dec << s_metadata_size << " bytes)\n";
        out << "libil2cpp Size:             0x" << std::hex << s_lib_size << " (" << std::dec << s_lib_size << " bytes)\n\n";
        out << "----------------------------------------------------\n";
        out << "HƯỚNG DẪN DÙNG CHO PC IL2CPPDUMPER:\n";
        out << "1. Khi Il2CppDumper trên PC hỏi 'Input dump address':\n";
        out << "   -> Hãy thử nhập: 0 (vì file libil2cpp.so đã được align từ offset 0)\n";
        out << "   -> Nếu không được, nhập địa chỉ Base: 0x" << std::hex << il2cpp_base << "\n\n";
        out << "2. Chú ý: Module Zygisk đã tự động xuất sẵn dump.cs\n";
        out << "   và script.json tại cùng thư mục files/ này!\n";
        out << "====================================================\n";
        out.flush();
        out.close();
        LOGI("Dump info saved to %s", infoPath.c_str());
    }

    LOGI("====================================================");
    LOGI("[Il2CppDumper] DUMP ADDRESS (il2cpp_base): 0x%llx", static_cast<unsigned long long>(il2cpp_base));
    LOGI("[Il2CppDumper] METADATA ADDRESS: 0x%llx", static_cast<unsigned long long>(s_metadata_addr));
    LOGI("[Il2CppDumper] For PC Il2CppDumper 'Input dump address': enter 0 or 0x%llx", static_cast<unsigned long long>(il2cpp_base));
    LOGI("====================================================");
}

void il2cpp_dump(const char *outDir) {
    LOGI("dumping...");
    
    
    auto filesDir = std::string(outDir).append("/files");
    ensure_directory(filesDir);

    size_t size = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    
    LOGI("Total assemblies: %zu", size);
    
    
    auto outPath = std::string(outDir).append("/files/dump.cs");
    std::ofstream outStream(outPath);
    
    if (!outStream.is_open()) {
        LOGE("Failed to open dump.cs");
        return;
    }
    
    
    for (int i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        outStream << "// Image " << i << ": " << il2cpp_image_get_name(image) << "\n";
    }
    
    int totalClasses = 0;
    int totalMethods = 0;
    
    
    for (int i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        
        const char* imageName = il2cpp_image_get_name(image);
        LOGI("Processing image %d: %s", i, imageName ? imageName : "NULL");
        
        if (!il2cpp_image_get_class_count || !il2cpp_image_get_class) {
            LOGW("il2cpp_image_get_class not available, skipping image %d", i);
            continue;
        }
        
        auto classCount = il2cpp_image_get_class_count(image);
        LOGI("  Class count: %zu", classCount);
        
        for (int j = 0; j < classCount; ++j) {
            auto klass = il2cpp_image_get_class(image, j);
            if (!klass) continue;
            
            auto type = il2cpp_class_get_type(const_cast<Il2CppClass*>(klass));
            if (!type) continue;
            
            
            outStream << "\n// Dll : " << (imageName ? imageName : "NULL");
            outStream << dump_type(type);
            
            totalClasses++;
            
            
            if (totalClasses % 100 == 0) {
                outStream.flush();
                LOGI("  Processed %d classes", totalClasses);
            }
        }
    }
    
    outStream.flush();
    outStream.close();
    
    LOGI("dump.cs done! Total %d classes", totalClasses);
    
    
    dump_script_json(outDir);

    
    LOGI("Starting dump_metadata...");
    dump_metadata(outDir);

    LOGI("Starting dump_libil2cpp...");
    dump_libil2cpp(outDir);
    
    
    save_dump_info(outDir);

    LOGI("dump done!");
}
