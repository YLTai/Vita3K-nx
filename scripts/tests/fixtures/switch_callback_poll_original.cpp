uint32_t process_callbacks(KernelState &kernel, SceUID thread_id) {
    ThreadStatePtr thread = kernel.get_thread(thread_id);
    if (!thread || thread->is_processing_callbacks)
        return 0;

    thread->is_processing_callbacks = true;
    uint32_t num_callbacks_processed = 0;
    std::vector<CallbackPtr> callbacks;
    {
        const std::lock_guard lock(kernel.mutex);
        callbacks = thread->callbacks;
    }
    for (const CallbackPtr &cb : callbacks) {
        if (cb->is_executable()) {
            std::string name = cb->get_name();
            cb->execute(kernel, [name]() {
                LOG_WARN("Callback with name {} requested to be deleted, but this is not supported yet!", name);
            });
            num_callbacks_processed++;
        }
    }
    thread->is_processing_callbacks = false;

    return num_callbacks_processed;
}
