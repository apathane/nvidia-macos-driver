/* Bounded submission flight recorder. No resource contents or application text. */
static pthread_mutex_t nvmtl_graph_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t serial, id, ns, bytes; const char *event; int result, split; }
    nvmtl_graph_ring[1024];
static uint64_t nvmtl_graph_serial, nvmtl_graph_next;
static int nvmtl_graph_trace = -1;
static void nvmtl_graph_record_locked(uint64_t id, const char *event, int result, int split, uint64_t bytes)
{
    const uint64_t serial = ++nvmtl_graph_serial;
    unsigned slot = (unsigned)((serial - 1) % 1024);
    nvmtl_graph_ring[slot].serial = serial;
    nvmtl_graph_ring[slot].id = id;
    nvmtl_graph_ring[slot].ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    nvmtl_graph_ring[slot].event = event;
    nvmtl_graph_ring[slot].result = result;
    nvmtl_graph_ring[slot].split = split;
    nvmtl_graph_ring[slot].bytes = bytes;
    if (nvmtl_graph_trace < 0) {
        const char *e = getenv("NVMTL_GRAPH_TRACE"); nvmtl_graph_trace = e && e[0] == '1';
    }
    if (nvmtl_graph_trace)
        nvlog("DAG seq=%llu id=%llu event=%s result=%d split=%d bytes=%llu ns=%llu",
            (unsigned long long)serial, (unsigned long long)id, event, result, split,
            (unsigned long long)bytes, (unsigned long long)nvmtl_graph_ring[slot].ns);
    if (result != VK_SUCCESS) {
        // Dump only on failure; normal rendering does no per-submission disk I/O.
        uint64_t first = serial > 1024 ? serial - 1023 : 1;
        for (uint64_t k = first; k <= serial; k++) {
            unsigned j = (unsigned)((k - 1) % 1024);
            nvlog("DAG seq=%llu id=%llu event=%s result=%d split=%d bytes=%llu ns=%llu",
                (unsigned long long)nvmtl_graph_ring[j].serial,
                (unsigned long long)nvmtl_graph_ring[j].id, nvmtl_graph_ring[j].event,
                nvmtl_graph_ring[j].result, nvmtl_graph_ring[j].split,
                (unsigned long long)nvmtl_graph_ring[j].bytes,
                (unsigned long long)nvmtl_graph_ring[j].ns);
        }
    }
}
static uint64_t nvmtl_graph_begin(int split, uint64_t bytes)
{
    pthread_mutex_lock(&nvmtl_graph_lock);
    uint64_t id = ++nvmtl_graph_next;
    nvmtl_graph_record_locked(id, "begin", VK_SUCCESS, split, bytes);
    pthread_mutex_unlock(&nvmtl_graph_lock);
    return id;
}
static void nvmtl_graph_event(uint64_t id, const char *event, int result)
{
    pthread_mutex_lock(&nvmtl_graph_lock);
    nvmtl_graph_record_locked(id, event, result, -1, 0);
    pthread_mutex_unlock(&nvmtl_graph_lock);
}
