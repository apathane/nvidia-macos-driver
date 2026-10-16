/* Process-local Vulkan pipeline cache with bounded, atomic persistence.
 * Included after Vulkan entry points; cache contents are owned by the backend.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
static VkPipelineCache g_pipeline_cache;
static pthread_mutex_t g_pipeline_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pipeline_cache_path[PATH_MAX];
static unsigned g_pipeline_cache_pending;
static PFN_vkCreatePipelineCache pCreatePipelineCache;
static PFN_vkGetPipelineCacheData pGetPipelineCacheData;
#define NVMTL_PIPELINE_CACHE_LIMIT (64u * 1024u * 1024u)

static void nvmtl_pipeline_cache_checkpoint(void) {
    // Called with the cache lock held. Delay serialization until the compilation
    // burst settles; disk writes do not run on the render/compilation caller.
    if (!g_pipeline_cache || !g_pipeline_cache_path[0] || g_pipeline_cache_pending) return;
    g_pipeline_cache_pending = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        size_t size = 0; void *data = NULL;
        pthread_mutex_lock(&g_pipeline_cache_lock);
        if (pGetPipelineCacheData(g_dev, g_pipeline_cache, &size, NULL) == VK_SUCCESS &&
            size && size <= NVMTL_PIPELINE_CACHE_LIMIT) {
            data = malloc(size);
            if (data && pGetPipelineCacheData(g_dev, g_pipeline_cache, &size, data) != VK_SUCCESS) {
                free(data); data = NULL;
            }
        }
        pthread_mutex_unlock(&g_pipeline_cache_lock);
        if (data) {
            char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.XXXXXX", g_pipeline_cache_path);
            int fd = mkstemp(tmp);
            if (fd >= 0) {
                fchmod(fd, 0600); size_t off = 0;
                while (off < size) {
                    ssize_t n = write(fd, (char *)data + off, size - off);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    off += (size_t)n;
                }
                close(fd);
                if (off != size || rename(tmp, g_pipeline_cache_path)) unlink(tmp);
            }
            free(data);
        }
        pthread_mutex_lock(&g_pipeline_cache_lock);
        g_pipeline_cache_pending = 0;
        pthread_mutex_unlock(&g_pipeline_cache_lock);
    });
}
static void nvmtl_pipeline_cache_init(const VkPhysicalDeviceProperties *properties) {
    if (getenv("NVMTL_NO_PIPELINE_CACHE")) return;
    pCreatePipelineCache = (PFN_vkCreatePipelineCache)pvkGetDeviceProcAddr(g_dev, "vkCreatePipelineCache");
    pGetPipelineCacheData = (PFN_vkGetPipelineCacheData)pvkGetDeviceProcAddr(g_dev, "vkGetPipelineCacheData");
    if (!pCreatePipelineCache || !pGetPipelineCacheData) return;
    char base[PATH_MAX], dir[PATH_MAX], uuid[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + i*2, 3, "%02x", properties->pipelineCacheUUID[i]);
    size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof base);
    if (length && length < sizeof base && snprintf(dir, sizeof dir, "%s/nvmtl-pipelines", base) < sizeof dir) {
        if (!mkdir(dir, 0700) || errno == EEXIST) {
            struct stat st;
            if (!lstat(dir, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0022)) {
                char app[48]; const char *prog = getprogname(); unsigned j = 0;
                for (; prog && prog[j] && j < sizeof app - 1; j++) {
                    char c = prog[j]; app[j] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
                }
                app[j] = 0;
                if (snprintf(g_pipeline_cache_path, sizeof g_pipeline_cache_path, "%s/%s-%x-%x-%x-%s.bin", dir,
                    app, properties->vendorID, properties->deviceID, properties->driverVersion, uuid) >= sizeof g_pipeline_cache_path)
                    g_pipeline_cache_path[0] = 0;
            }
        }
    }
    void *data = NULL; size_t size = 0;
    int fd = g_pipeline_cache_path[0] ? open(g_pipeline_cache_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (fd >= 0) {
        struct stat st;
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
            st.st_size >= 32 && st.st_size <= NVMTL_PIPELINE_CACHE_LIMIT) {
            size = (size_t)st.st_size; data = malloc(size); size_t off = 0;
            while (data && off < size) {
                ssize_t n = read(fd, (char *)data + off, size - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                off += (size_t)n;
            }
            if (off != size) { free(data); data = NULL; size = 0; }
        }
        close(fd);
    }
    // Validate the standard version-one header before passing bytes to NVK.
    if (data) {
        uint32_t header[4]; memcpy(header, data, sizeof header);
        if (header[0] < 32 || header[0] > size || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            header[2] != properties->vendorID || header[3] != properties->deviceID ||
            memcmp((char *)data + 16, properties->pipelineCacheUUID, VK_UUID_SIZE)) {
            free(data); data = NULL; size = 0;
        }
    }
    VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, data };
    VkResult result = pCreatePipelineCache(g_dev, &info, NULL, &g_pipeline_cache);
    if (result != VK_SUCCESS && size) {
        info.initialDataSize = 0; info.pInitialData = NULL;
        result = pCreatePipelineCache(g_dev, &info, NULL, &g_pipeline_cache);
    }
    free(data);
    if (result != VK_SUCCESS) g_pipeline_cache = VK_NULL_HANDLE;
    nvlog("pipeline cache: %s, %zu input bytes, persistence %s", g_pipeline_cache ? "enabled" : "unavailable",
          size, g_pipeline_cache_path[0] ? "per-user/per-backend" : "disabled");
}
