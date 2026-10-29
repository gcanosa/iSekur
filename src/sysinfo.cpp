// What /statusbar shows about this machine: CPU, memory, battery. Kept out of
// core.h's way: macOS's headers define their own Style.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

long long now_ms();

// cpu ticks (busy, total) since boot; false if the system won't say
static bool cpu_ticks(unsigned long long& busy, unsigned long long& total) {
#ifdef __APPLE__
    host_cpu_load_info_data_t d;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&d, &n) != KERN_SUCCESS) return false;
    unsigned long long idle = d.cpu_ticks[CPU_STATE_IDLE];
    total = idle + d.cpu_ticks[CPU_STATE_USER] + d.cpu_ticks[CPU_STATE_SYSTEM] + d.cpu_ticks[CPU_STATE_NICE];
#else
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long v[8] = {};
    int got = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (got < 4) return false;
    unsigned long long idle = v[3] + v[4];  // idle + iowait
    total = 0;
    for (auto x : v) total += x;
#endif
    busy = total - idle;
    return true;
}

// CPU use since the previous call at least a second ago, -1 until there are two samples
int cpu_pct() {
    static unsigned long long pb, pt;
    static long long at;
    static int last = -1;
    long long now = now_ms();
    if (at && now - at < 1000) return last;
    unsigned long long b, t;
    if (!cpu_ticks(b, t)) return -1;
    if (at && t > pt) last = (int)((b - pb) * 100 / (t - pt));
    pb = b, pt = t, at = now;
    return last;
}

// bytes free (available to programs) and total
bool mem_free(unsigned long long& avail, unsigned long long& total) {
#ifdef __APPLE__
    size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0)) return false;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n) != KERN_SUCCESS) return false;
    // ponytail: free + inactive + speculative, close to what Activity Monitor calls not used
    avail = (unsigned long long)(vm.free_count + vm.inactive_count + vm.speculative_count) * (unsigned long long)vm_kernel_page_size;
    return true;
#else
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    char k[64];
    unsigned long long v, mt = 0, ma = 0, mf = 0;
    while (fscanf(f, "%63s %llu kB", k, &v) == 2) {
        if (!strcmp(k, "MemTotal:")) mt = v;
        else if (!strcmp(k, "MemAvailable:")) ma = v;
        else if (!strcmp(k, "MemFree:")) mf = v;
    }
    fclose(f);
    total = mt * 1024, avail = (ma ? ma : mf) * 1024;  // no MemAvailable before Linux 3.14
    return mt > 0;
#endif
}

// battery charge 0-100, -1 without a battery (a desktop, a server)
int battery_pct() {
#ifdef __APPLE__
    int pct = -1;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return -1;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && pct < 0; i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        CFStringRef type = d ? (CFStringRef)CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey)) : nullptr;
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
        int cur = 0, max = 0;
        auto num = [&](CFStringRef k, int& out) {
            CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(d, k);
            return n && CFNumberGetValue(n, kCFNumberIntType, &out);
        };
        if (num(CFSTR(kIOPSCurrentCapacityKey), cur) && num(CFSTR(kIOPSMaxCapacityKey), max) && max > 0) pct = cur * 100 / max;
    }
    if (list) CFRelease(list);
    CFRelease(info);
    return pct;
#else
    for (const char* f : {"/sys/class/power_supply/BAT0/capacity", "/sys/class/power_supply/BAT1/capacity"}) {
        FILE* fp = fopen(f, "r");
        if (!fp) continue;
        int v = -1;
        if (fscanf(fp, "%d", &v) != 1) v = -1;
        fclose(fp);
        if (v >= 0) return std::min(v, 100);
    }
    return -1;
#endif
}
