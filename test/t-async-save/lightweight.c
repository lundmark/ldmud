#pragma lightweight, no_warn_lightweight

#if __EFUN_DEFINED__(async_save_object)
void submit(closure callback)
{
    async_save_object("lightweight", callback);
}
#endif
