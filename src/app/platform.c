#include "core.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
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
    snprintf(h->os, sizeof h->os, "%s %s", u.sysname, u.release);
    snprintf(h->arch, sizeof h->arch, "%s", u.machine);
    snprintf(h->name, sizeof h->name, "%s %s", u.sysname, u.machine);
    long cores      = sysconf(_SC_NPROCESSORS_ONLN);
    h->cores        = cores > 0 ? (unsigned) cores : 1;
    h->logical_cpus = cores > 0 ? (unsigned) cores : 0;
    struct statvfs disk;
    if (statvfs(directory, &disk) == 0)
        h->disk_known = !ckd_mul(&h->disk, (uint64_t) disk.f_bavail, (uint64_t) disk.f_frsize);
#ifdef __APPLE__
    char   version[64] = "";
    size_t n           = sizeof version;
    if (sysctlbyname("kern.osproductversion", version, &n, nullptr, 0) == 0)
        snprintf(h->os, sizeof h->os, "macOS %s", version);
    n = sizeof h->ram;
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
    f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12))
                continue;
            char *name                  = line + 12;
            name[strcspn(name, "\r\n")] = 0;
            size_t length               = strlen(name);
            if (length >= 2 && (name[0] == '\"' || name[0] == '\'') &&
                name[length - 1] == name[0]) {
                name[length - 1] = 0;
                ++name;
            }
            snprintf(h->os, sizeof h->os, "%s", name);
            break;
        }
        fclose(f);
    }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10))
                continue;
            char *name = strchr(line, ':');
            if (!name)
                continue;
            ++name;
            while (*name == ' ' || *name == '\t')
                ++name;
            name[strcspn(name, "\r\n")] = 0;
            snprintf(h->name, sizeof h->name, "%s", name);
            break;
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

/* Compatibility identity, not signature verification. Mach-O signatures and
 * their size metadata change during notarized UI-only repackaging. Hash every
 * executable/link-edit payload byte while canonicalizing only that envelope.
 * Unknown formats use the ordinary file hash; model integrity always does. */
bool app_engine_sha256(const char *path, char out[static 65]) {
#ifndef __APPLE__
    return app_sha256(path, out);
#else
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat   before, after;
    unsigned char header[65536], block[65536], digest[32];
    bool          parsed = false, ok = false;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || pread(fd, header, 32, 0) != 32)
        goto end;
    uint32_t words[8];
    memcpy(words, header, sizeof words);
    if (words[0] != 0xfeedfacf || words[4] > 4096 || words[5] > sizeof header - 32)
        goto end;
    size_t length = 32 + (size_t) words[5];
    if (pread(fd, header, length, 0) != (ssize_t) length)
        goto end;
    size_t   offset = 32, signature_command = 0, link = 0;
    uint32_t signature = 0, signature_size = 0;
    for (uint32_t i = 0; i < words[4]; i++) {
        uint32_t command[2];
        if (offset + sizeof command > length)
            goto end;
        memcpy(command, header + offset, sizeof command);
        if (command[1] < 8 || command[1] > length - offset)
            goto end;
        if (command[0] == 0x1d) {
            if (command[1] != 16 || signature_command)
                goto end;
            signature_command = offset;
            memcpy(&signature, header + offset + 8, 4);
            memcpy(&signature_size, header + offset + 12, 4);
        }
        if (command[0] == 0x19 && command[1] >= 72 &&
            !memcmp(header + offset + 8, "__LINKEDIT\0\0\0\0\0\0", 16))
            link = offset;
        offset += command[1];
    }
    if (offset != length || !signature_command || !link || signature < length || !signature_size ||
        (uint64_t) signature + signature_size != (uint64_t) before.st_size)
        goto end;
    memset(header + signature_command + 8, 0, 8);
    memset(header + link + 32, 0, 8); /* Signature-dependent virtual segment size. */
    memset(header + link + 48, 0, 8); /* Signature-dependent file segment size. */
    parsed = true;
    CC_SHA256_CTX hash;
    ok = CC_SHA256_Init(&hash) == 1 && CC_SHA256_Update(&hash, header, (CC_LONG) length) == 1;
    for (uint64_t pos = length; ok && pos < signature;) {
        size_t  wanted = signature - pos < sizeof block ? (size_t) (signature - pos) : sizeof block;
        ssize_t n      = pread(fd, block, wanted, (off_t) pos);
        if (n != (ssize_t) wanted) {
            ok = false;
            break;
        }
        ok = CC_SHA256_Update(&hash, block, (CC_LONG) n) == 1;
        pos += (size_t) n;
    }
    if (ok)
        ok = fstat(fd, &after) == 0 && before.st_size == after.st_size &&
             before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
             before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
             CC_SHA256_Final(digest, &hash) == 1;
end:
    close(fd);
    if (!parsed)
        return app_sha256(path, out);
    if (ok)
        for (size_t i = 0; i < 32; i++)
            snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return ok;
#endif
}
