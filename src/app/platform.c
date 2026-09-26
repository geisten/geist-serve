#include "core.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <stdckdint.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <openssl/evp.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

#ifndef __APPLE__
/* Match the immutable engine's generic Linux compilation baseline. */
static bool linux_cpu_supported(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("x86-64-v3") != 0;
#elif defined(__aarch64__)
    const unsigned long required = HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP | HWCAP_ASIMDDP;
    return (getauxval(AT_HWCAP) & required) == required;
#else
    return false;
#endif
}
#endif

bool app_hardware_read(struct app_hardware *h, const char *directory) {
    *h = (struct app_hardware) {};
    struct utsname u;
    if (uname(&u) != 0)
        return false;
    snprintf(h->arch, sizeof h->arch, "%s", u.machine);
    snprintf(h->name, sizeof h->name, "%s %s", u.sysname, u.machine);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    h->cores   = cores > 0 ? (unsigned) cores : 1;
    struct statvfs disk;
    if (statvfs(directory, &disk) == 0)
        h->disk_known = !ckd_mul(&h->disk, (uint64_t) disk.f_bavail, (uint64_t) disk.f_frsize);
#ifdef __APPLE__
    size_t n = sizeof h->ram;
    if (sysctlbyname("hw.memsize", &h->ram, &n, nullptr, 0) != 0)
        return false;
    n = sizeof h->name;
    if (sysctlbyname("machdep.cpu.brand_string", h->name, &n, nullptr, 0) != 0)
        snprintf(h->name, sizeof h->name, "Mac (%s)", h->arch);
    h->supported = strcmp(u.machine, "arm64") == 0;
    if (h->supported)
        h->device = APP_APPLE_SILICON;
    int pcores = 0;
    n          = sizeof pcores;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &n, nullptr, 0) == 0 && pcores > 0)
        h->cores = (unsigned) pcores;
    /* Free + inactive is a conservative snapshot, not an allocation guarantee. */
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    mach_port_t            host  = mach_host_self();
    vm_size_t              page  = 0;
    if (host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t) &vm, &count) == KERN_SUCCESS) {
        h->available       = ((uint64_t) vm.free_count + vm.inactive_count) * page;
        h->available_known = true;
    }
    mach_port_deallocate(mach_task_self(), host);
#else
    h->supported = linux_cpu_supported();
    FILE *f      = fopen("/proc/meminfo", "r");
    if (f) {
        char               line[256];
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
                h->ram = kb * 1024;
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                h->available       = kb * 1024;
                h->available_known = true;
            }
        }
        fclose(f);
    }
    f = fopen("/proc/device-tree/model", "r");
    if (f) {
        size_t got   = fread(h->name, 1, sizeof h->name - 1, f);
        h->name[got] = 0;
        fclose(f);
        if (strstr(h->name, "Raspberry Pi 5"))
            h->device = APP_PI5;
    }
#endif
    return h->ram != 0;
}

bool app_sha256_interruptible(const char *path, char out[static 65], bool (*cancel)(void)) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    unsigned char block[65536], digest[32];
    bool          ok = true;
#ifdef __APPLE__
    CC_SHA256_CTX context;
    ok = CC_SHA256_Init(&context) == 1;
#else
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    ok                  = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
#endif
    while (ok) {
        if (cancel && cancel()) {
            ok = false;
            break;
        }
        ssize_t n = read(fd, block, sizeof block);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
#ifdef __APPLE__
        ok = CC_SHA256_Update(&context, block, (CC_LONG) n) == 1;
#else
        ok = EVP_DigestUpdate(context, block, (size_t) n) == 1;
#endif
    }
#ifdef __APPLE__
    if (ok)
        ok = CC_SHA256_Final(digest, &context) == 1;
#else
    unsigned length = 0;
    if (ok)
        ok = EVP_DigestFinal_ex(context, digest, &length) == 1 && length == 32;
    EVP_MD_CTX_free(context);
#endif
    close(fd);
    if (ok)
        for (size_t i = 0; i < 32; ++i)
            snprintf(out + i * 2, 3, "%02x", digest[i]);
    return ok;
}

bool app_sha256(const char *path, char out[static 65]) {
    return app_sha256_interruptible(path, out, nullptr);
}
