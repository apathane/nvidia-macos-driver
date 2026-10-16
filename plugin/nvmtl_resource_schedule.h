/* All accesses are exclusive until read/write and indirect alias coverage exist.
 * Protected by nvmtl_submit_mutex. Entries own copied keys, not ObjC pointers. */
static struct { VkFence fence; unsigned lane, count; uint64_t value; uintptr_t keys[64]; }
    nvmtl_active_resources[128];
static int nvmtl_parallel_disabled;
static uint64_t nvmtl_lane_value[2], nvmtl_lane_completed[2];
static void nvmtl_resource_completed(unsigned i)
{
    unsigned lane = nvmtl_active_resources[i].lane;
    if (nvmtl_active_resources[i].value > nvmtl_lane_completed[lane])
        nvmtl_lane_completed[lane] = nvmtl_active_resources[i].value;
    nvmtl_active_resources[i].fence = VK_NULL_HANDLE;
}
static VkResult nvmtl_resource_submit(VkQueue queue, const VkSubmitInfo *source,
                                     VkFence fence, unsigned lane, int final)
{
    if (!g_resource_timeline[0]) return pvkQueueSubmit(queue, 1, source, fence);
    if (source->waitSemaphoreCount > 1 || source->signalSemaphoreCount > 1 || source->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSubmitInfo info = *source;
    VkSemaphore waits[2], signals[2];
    VkPipelineStageFlags stages[2];
    uint64_t wait_values[2] = {0}, signal_values[2] = {0};
    for (unsigned i = 0; i < source->waitSemaphoreCount; i++) {
        waits[i] = source->pWaitSemaphores[i]; stages[i] = source->pWaitDstStageMask[i];
    }
    for (unsigned i = 0; i < source->signalSemaphoreCount; i++) signals[i] = source->pSignalSemaphores[i];
    if (nvmtl_lane_completed[1 - lane]) {
        unsigned i = info.waitSemaphoreCount++;
        waits[i] = g_resource_timeline[1 - lane]; stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        wait_values[i] = nvmtl_lane_completed[1 - lane];
    }
    if (final) {
        unsigned i = info.signalSemaphoreCount++;
        signals[i] = g_resource_timeline[lane]; signal_values[i] = ++nvmtl_lane_value[lane];
    }
    VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        NULL, info.waitSemaphoreCount, wait_values, info.signalSemaphoreCount, signal_values };
    info.pNext = &timeline; info.pWaitSemaphores = waits; info.pWaitDstStageMask = stages;
    info.pSignalSemaphores = signals;
    return pvkQueueSubmit(queue, 1, &info, fence);
}
static int nvmtl_resource_conflict(const nvk_cmdbuf *c, unsigned i)
{
    if (!c->resource_key_count || !nvmtl_active_resources[i].count) return 1;
    for (unsigned a = 0; a < c->resource_key_count; a++)
        for (unsigned b = 0; b < nvmtl_active_resources[i].count; b++)
            if (c->resource_keys[a] == nvmtl_active_resources[i].keys[b]) return 1;
    return 0;
}
static VkResult nvmtl_resource_lane(nvk_cmdbuf *c, int has_prologue, unsigned *lane)
{
    unsigned count[2] = {0}, free_slots = 0;
    int primary_conflict = 0;
    const char *off = getenv("NVMTL_NO_RESOURCE_PARALLEL");
    int eligible = g_resource_timeline[0] && g_copy_queue && !has_prologue && c->resource_key_count &&
        !nvmtl_parallel_disabled && !(off && off[0] == '1');
    static uint64_t seen, keyed, upload_parts;
    seen++; keyed += c->resource_key_count != 0; upload_parts += has_prologue != 0;
    if (seen == 1 || (seen & 4095) == 0)
        nvlog("resource scheduler: census %llu submits, %llu tracked, %llu with uploads; timelines=%d disabled=%d",
              (unsigned long long)seen, (unsigned long long)keyed, (unsigned long long)upload_parts,
              g_resource_timeline[0] != VK_NULL_HANDLE, nvmtl_parallel_disabled);
    *lane = 0;
    for (unsigned i = 0; i < 128; i++) {
        if (nvmtl_active_resources[i].fence) {
            VkResult r = pvkWaitForFences(g_dev, 1, &nvmtl_active_resources[i].fence, VK_TRUE, 0);
            if (r == VK_SUCCESS) nvmtl_resource_completed(i);
            else if (r != VK_TIMEOUT) return r;
        }
        if (!nvmtl_active_resources[i].fence) { free_slots++; continue; }
        unsigned q = nvmtl_active_resources[i].lane;
        count[q]++;
        if (!q && nvmtl_resource_conflict(c, i)) primary_conflict = 1;
    }
    if (!free_slots) { nvmtl_parallel_disabled = 1; eligible = 0; }
    if (eligible && !primary_conflict && count[0] > count[1]) {
        *lane = 1;
        static uint64_t parallel;
        if (++parallel == 1 || (parallel & 1023) == 0)
            nvlog("resource scheduler: %llu independent submissions assigned to lane 1", (unsigned long long)parallel);
    }
    // Same-queue FIFO orders conflicts. Cross-queue conflicts wait for actual
    // completion; unknown accesses and upload prologues drain the other lane.
    for (unsigned i = 0; i < 128; i++) {
        if (!nvmtl_active_resources[i].fence || nvmtl_active_resources[i].lane == *lane) continue;
        if (!has_prologue && nvmtl_resource_conflict(c, i) == 0) continue;
        VkResult r = pvkWaitForFences(g_dev, 1, &nvmtl_active_resources[i].fence, VK_TRUE,
            (uint64_t)nvmtl_gpu_wait_budget_ms() * 1000000ull);
        if (r != VK_SUCCESS) return r;
        nvmtl_resource_completed(i);
    }
    return VK_SUCCESS;
}
static void nvmtl_resource_register(nvk_cmdbuf *c, VkFence fence, unsigned lane, int opaque)
{
    for (unsigned i = 0; i < 128; i++) if (!nvmtl_active_resources[i].fence) {
        nvmtl_active_resources[i].fence = fence;
        nvmtl_active_resources[i].lane = lane;
        nvmtl_active_resources[i].value = nvmtl_lane_value[lane];
        nvmtl_active_resources[i].count = opaque ? 0 : c->resource_key_count;
        memcpy(nvmtl_active_resources[i].keys, c->resource_keys,
            nvmtl_active_resources[i].count * sizeof(uintptr_t));
        return;
    }
    nvmtl_parallel_disabled = 1;
}
static void nvmtl_resource_forget(VkFence fence)
{
    pthread_mutex_lock(&nvmtl_submit_mutex);
    for (unsigned i = 0; i < 128; i++)
        if (nvmtl_active_resources[i].fence == fence) nvmtl_resource_completed(i);
    pthread_mutex_unlock(&nvmtl_submit_mutex);
}
