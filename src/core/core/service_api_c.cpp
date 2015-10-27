/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
# include <dsn/service_api_c.h>
# include <dsn/tool_api.h>
# include <dsn/internal/enum_helper.h>
# include <dsn/cpp/auto_codes.h>
# include <dsn/cpp/serialization.h>
# include <dsn/internal/task_spec.h>
# include <dsn/internal/zlock_provider.h>
# include <dsn/internal/nfs.h>
# include <dsn/internal/env_provider.h>
# include <dsn/internal/zookeeper_provider.h>
# include <dsn/internal/factory_store.h>
# include <dsn/internal/task.h>
# include <dsn/internal/singleton_store.h>
# include <dsn/internal/configuration.h>

# include "command_manager.h"
# include "service_engine.h"
# include "rpc_engine.h"
# include "disk_engine.h"
# include "coredump.h"
# include "crc.h"
# include <fstream>

# ifndef _WIN32
# include <signal.h>
# endif

//
// global state
//
static struct _all_info_
{
    int                                                       magic;
    bool                                                      engine_ready;
    bool                                                      config_completed;
    ::dsn::tools::tool_app                                    *tool;
    ::dsn::configuration_ptr                                  config;
    ::dsn::service_engine                                     *engine;
    std::vector<::dsn::task_spec*>                            task_specs;
    ::dsn::memory_provider                                    *memory;

    bool is_config_completed() const {
        return magic == 0xdeadbeef && config_completed;
    }

    bool is_engine_ready() const {
        return magic == 0xdeadbeef && engine_ready;
    }

} dsn_all;

//------------------------------------------------------------------------------
//
// common types
//
//------------------------------------------------------------------------------
struct dsn_error_placeholder {};
class error_code_mgr : public ::dsn::utils::customized_id_mgr < dsn_error_placeholder >
{
public:
    error_code_mgr()
    {
        auto err = register_id("ERR_OK"); // make sure ERR_OK is always registered first
        dassert(0 == err, "");
    }
};

DSN_API dsn_error_t dsn_error_register(const char* name)
{
    return static_cast<dsn_error_t>(error_code_mgr::instance().register_id(name));
}

DSN_API const char* dsn_error_to_string(dsn_error_t err)
{
    return error_code_mgr::instance().get_name(static_cast<int>(err));
}

// use ::dsn::threadpool_code2; for parsing purpose
DSN_API dsn_threadpool_code_t dsn_threadpool_code_register(const char* name)
{
    dassert(!dsn_all.is_config_completed(), 
        "thread pool code '%s' must be registered before the service app role is registered",
        name);

    return static_cast<dsn_threadpool_code_t>(
        ::dsn::utils::customized_id_mgr<::dsn::threadpool_code2_>::instance().register_id(name)
        );
}

DSN_API const char* dsn_threadpool_code_to_string(dsn_threadpool_code_t pool_code)
{
    return ::dsn::utils::customized_id_mgr<::dsn::threadpool_code2_>::instance().get_name(static_cast<int>(pool_code));
}

DSN_API dsn_threadpool_code_t dsn_threadpool_code_from_string(const char* s, dsn_threadpool_code_t default_code)
{
    auto r = ::dsn::utils::customized_id_mgr<::dsn::threadpool_code2_>::instance().get_id(s);
    return r == -1 ? default_code : r;
}

DSN_API int dsn_threadpool_code_max()
{
    return ::dsn::utils::customized_id_mgr<::dsn::threadpool_code2_>::instance().max_value();
}

DSN_API int dsn_threadpool_get_current_tid()
{
    return ::dsn::utils::get_current_tid();
}

struct task_code_placeholder { };
DSN_API dsn_task_code_t dsn_task_code_register(const char* name, dsn_task_type_t type,
    dsn_task_priority_t pri, dsn_threadpool_code_t pool)
{
    dassert(!dsn_all.is_config_completed(),
        "task code '%s' must be registered before the service app role is registered",
        name);

    auto r = static_cast<dsn_task_code_t>(::dsn::utils::customized_id_mgr<task_code_placeholder>::instance().register_id(name));
    ::dsn::task_spec::register_task_code(r, type, pri, pool);
    return r;
}

DSN_API void dsn_task_code_query(dsn_task_code_t code, dsn_task_type_t *ptype, dsn_task_priority_t *ppri, dsn_threadpool_code_t *ppool)
{
    auto sp = ::dsn::task_spec::get(code);
    dassert(sp != nullptr, "");
    if (ptype) *ptype = sp->type;
    if (ppri) *ppri = sp->priority;
    if (ppool) *ppool = sp->pool_code;
}

DSN_API void dsn_task_code_set_threadpool(dsn_task_code_t code, dsn_threadpool_code_t pool)
{
    auto sp = ::dsn::task_spec::get(code);
    dassert(sp != nullptr, "");
    sp->pool_code = pool;
}

DSN_API void dsn_task_code_set_priority(dsn_task_code_t code, dsn_task_priority_t pri)
{
    auto sp = ::dsn::task_spec::get(code);
    dassert(sp != nullptr, "");
    sp->priority = pri;
}

DSN_API const char* dsn_task_code_to_string(dsn_task_code_t code)
{
    return ::dsn::utils::customized_id_mgr<task_code_placeholder>::instance().get_name(static_cast<int>(code));
}

DSN_API dsn_task_code_t dsn_task_code_from_string(const char* s, dsn_task_code_t default_code)
{
    auto r = ::dsn::utils::customized_id_mgr<task_code_placeholder>::instance().get_id(s);
    return r == -1 ? default_code : r;
}

DSN_API int dsn_task_code_max()
{
    return ::dsn::utils::customized_id_mgr<task_code_placeholder>::instance().max_value();
}

DSN_API const char* dsn_task_type_to_string(dsn_task_type_t tt)
{
    return enum_to_string(tt);
}

DSN_API const char* dsn_task_priority_to_string(dsn_task_priority_t tt)
{
    return enum_to_string(tt);
}

DSN_API const char* dsn_config_get_value_string(const char* section, const char* key, const char* default_value, const char* dsptr)
{
    return dsn_all.config->get_string_value(section, key, default_value, dsptr);
}

DSN_API bool dsn_config_get_value_bool(const char* section, const char* key, bool default_value, const char* dsptr)
{
    return dsn_all.config->get_value<bool>(section, key, default_value, dsptr);
}

DSN_API uint64_t dsn_config_get_value_uint64(const char* section, const char* key, uint64_t default_value, const char* dsptr)
{
    return dsn_all.config->get_value<uint64_t>(section, key, default_value, dsptr);
}

DSN_API double dsn_config_get_value_double(const char* section, const char* key, double default_value, const char* dsptr)
{
    return dsn_all.config->get_value<double>(section, key, default_value, dsptr);
}

DSN_API int dsn_config_get_all_keys(const char* section, const char** buffers, /*inout*/ int* buffer_count) // return all key count (may greater than buffer_count)
{
    std::vector<const char*> keys;
    dsn_all.config->get_all_keys(section, keys);
    int kcount = (int)keys.size();

    if (*buffer_count > kcount)
        *buffer_count = kcount;

    for (int i = 0; i < *buffer_count; i++)
    {
        buffers[i] = keys[i];
    }

    return kcount;
}

DSN_API void dsn_coredump()
{
    ::dsn::utils::coredump::write(); 
    ::abort();
}

DSN_API uint32_t dsn_crc32_compute(const void* ptr, size_t size, uint32_t init_crc)
{
    return ::dsn::utils::crc32::compute(ptr, size, init_crc);
}

DSN_API uint32_t dsn_crc32_concatenate(uint32_t xy_init, uint32_t x_init, uint32_t x_final, size_t x_size, uint32_t y_init, uint32_t y_final, size_t y_size)
{
    return ::dsn::utils::crc32::concatenate(
        0,
        x_init, x_final, (uint64_t)x_size,
        y_init, y_final, (uint64_t)y_size
        );
}

//------------------------------------------------------------------------------
//
// tasking - asynchronous tasks and timers tasks executed in target thread pools
// (configured in config files)
// [task.RPC_PREPARE
// // TODO: what can be configured for a task
//
// [threadpool.THREAD_POOL_REPLICATION]
// // TODO: what can be configured for a thread pool
//
//------------------------------------------------------------------------------
DSN_API dsn_task_t dsn_task_create(dsn_task_code_t code, dsn_task_handler_t cb, void* param, int hash)
{
    auto t = new ::dsn::task_c(code, cb, param, hash);
    return t;
}

DSN_API dsn_task_t dsn_task_create_timer(dsn_task_code_t code, dsn_task_handler_t cb, 
    void* param, int hash, int interval_milliseconds)
{
    auto t = new ::dsn::timer_task(code, cb, param, interval_milliseconds, hash);
    return t;
}

DSN_API dsn_task_tracker_t dsn_task_tracker_create(int task_bucket_count)
{
    return (dsn_task_tracker_t)(new ::dsn::task_tracker(task_bucket_count));
}

DSN_API void dsn_task_tracker_destroy(dsn_task_tracker_t tracker)
{
    delete ((::dsn::task_tracker*)tracker);
}

DSN_API void dsn_task_tracker_cancel_all(dsn_task_tracker_t tracker)
{
    ((::dsn::task_tracker*)tracker)->cancel_outstanding_tasks();
}

DSN_API void dsn_task_tracker_wait_all(dsn_task_tracker_t tracker)
{
    ((::dsn::task_tracker*)tracker)->wait_outstanding_tasks();
}

DSN_API void dsn_task_call(dsn_task_t task, dsn_task_tracker_t tracker, int delay_milliseconds)
{
    auto t = ((::dsn::task*)(task));
    dassert(t->spec().type == TASK_TYPE_COMPUTE, "must be common or timer task");

    t->set_tracker((::dsn::task_tracker*)tracker);
    t->set_delay(delay_milliseconds);
    t->enqueue();
}

DSN_API void dsn_task_add_ref(dsn_task_t task)
{
    ((::dsn::task*)(task))->add_ref();
}

DSN_API void dsn_task_release_ref(dsn_task_t task)
{
    ((::dsn::task*)(task))->release_ref();
}

DSN_API bool dsn_task_cancel(dsn_task_t task, bool wait_until_finished)
{
    return ((::dsn::task*)(task))->cancel(wait_until_finished);
}

DSN_API bool dsn_task_cancel2(dsn_task_t task, bool wait_until_finished, bool* finished)
{
    return ((::dsn::task*)(task))->cancel(wait_until_finished, finished);
}

DSN_API bool dsn_task_wait(dsn_task_t task)
{
    return ((::dsn::task*)(task))->wait();
}

DSN_API bool dsn_task_wait_timeout(dsn_task_t task, int timeout_milliseconds)
{
    return ((::dsn::task*)(task))->wait(timeout_milliseconds);
}

DSN_API dsn_error_t dsn_task_error(dsn_task_t task)
{
    return ((::dsn::task*)(task))->error().get();
}

//------------------------------------------------------------------------------
//
// synchronization - concurrent access and coordination among threads
//
//------------------------------------------------------------------------------
DSN_API dsn_handle_t dsn_exlock_create(bool recursive)
{
    if (recursive)
    {
        ::dsn::lock_provider* last = ::dsn::utils::factory_store<::dsn::lock_provider>::create(
            ::dsn::service_engine::fast_instance().spec().lock_factory_name.c_str(), PROVIDER_TYPE_MAIN, nullptr);

        // TODO: perf opt by saving the func ptrs somewhere
        for (auto& s : ::dsn::service_engine::fast_instance().spec().lock_aspects)
        {
            last = ::dsn::utils::factory_store<::dsn::lock_provider>::create(s.c_str(), PROVIDER_TYPE_ASPECT, last);
        }

        return (dsn_handle_t)dynamic_cast<::dsn::ilock*>(last);
    }
    else
    {
        ::dsn::lock_nr_provider* last = ::dsn::utils::factory_store<::dsn::lock_nr_provider>::create(
            ::dsn::service_engine::fast_instance().spec().lock_nr_factory_name.c_str(), PROVIDER_TYPE_MAIN, nullptr);

        // TODO: perf opt by saving the func ptrs somewhere
        for (auto& s : ::dsn::service_engine::fast_instance().spec().lock_nr_aspects)
        {
            last = ::dsn::utils::factory_store<::dsn::lock_nr_provider>::create(s.c_str(), PROVIDER_TYPE_ASPECT, last);
        }

        return (dsn_handle_t)dynamic_cast<::dsn::ilock*>(last);
    }
}

DSN_API void dsn_exlock_destroy(dsn_handle_t l)
{
    delete (::dsn::ilock*)(l);
}

DSN_API void dsn_exlock_lock(dsn_handle_t l)
{
    ((::dsn::ilock*)(l))->lock();
    ::dsn::lock_checker::zlock_exclusive_count++;
}

DSN_API bool dsn_exlock_try_lock(dsn_handle_t l)
{
    auto r = ((::dsn::ilock*)(l))->try_lock();
    if (r)
    {
        ::dsn::lock_checker::zlock_exclusive_count++;
    }
    return r;
}

DSN_API void dsn_exlock_unlock(dsn_handle_t l)
{
    ::dsn::lock_checker::zlock_exclusive_count--;
    ((::dsn::ilock*)(l))->unlock();
}

// non-recursive rwlock
DSN_API dsn_handle_t dsn_rwlock_nr_create()
{
    ::dsn::rwlock_nr_provider* last = ::dsn::utils::factory_store<::dsn::rwlock_nr_provider>::create(
        ::dsn::service_engine::fast_instance().spec().rwlock_nr_factory_name.c_str(), PROVIDER_TYPE_MAIN, nullptr);

    // TODO: perf opt by saving the func ptrs somewhere
    for (auto& s : ::dsn::service_engine::fast_instance().spec().rwlock_nr_aspects)
    {
        last = ::dsn::utils::factory_store<::dsn::rwlock_nr_provider>::create(s.c_str(), PROVIDER_TYPE_ASPECT, last);
    }
    return (dsn_handle_t)(last);
}

DSN_API void dsn_rwlock_nr_destroy(dsn_handle_t l)
{
    delete (::dsn::rwlock_nr_provider*)(l);
}

DSN_API void dsn_rwlock_nr_lock_read(dsn_handle_t l)
{
    ((::dsn::rwlock_nr_provider*)(l))->lock_read();
    ::dsn::lock_checker::zlock_shared_count++;
}

DSN_API void dsn_rwlock_nr_unlock_read(dsn_handle_t l)
{
    ::dsn::lock_checker::zlock_shared_count--;
    ((::dsn::rwlock_nr_provider*)(l))->unlock_read();
}

DSN_API void dsn_rwlock_nr_lock_write(dsn_handle_t l)
{
    ((::dsn::rwlock_nr_provider*)(l))->lock_write();
    ::dsn::lock_checker::zlock_exclusive_count++;
}

DSN_API void dsn_rwlock_nr_unlock_write(dsn_handle_t l)
{
    ::dsn::lock_checker::zlock_exclusive_count--;
    ((::dsn::rwlock_nr_provider*)(l))->unlock_write();
}


DSN_API dsn_handle_t dsn_semaphore_create(int initial_count)
{
    ::dsn::semaphore_provider* last = ::dsn::utils::factory_store<::dsn::semaphore_provider>::create(
        ::dsn::service_engine::fast_instance().spec().semaphore_factory_name.c_str(), PROVIDER_TYPE_MAIN, initial_count, nullptr);

    // TODO: perf opt by saving the func ptrs somewhere
    for (auto& s : ::dsn::service_engine::fast_instance().spec().semaphore_aspects)
    {
        last = ::dsn::utils::factory_store<::dsn::semaphore_provider>::create(
            s.c_str(), PROVIDER_TYPE_ASPECT, initial_count, last);
    }
    return (dsn_handle_t)(last);
}

DSN_API void dsn_semaphore_destroy(dsn_handle_t s)
{
    delete (::dsn::semaphore_provider*)(s);
}

DSN_API void dsn_semaphore_signal(dsn_handle_t s, int count)
{
    ((::dsn::semaphore_provider*)(s))->signal(count);
}

DSN_API void dsn_semaphore_wait(dsn_handle_t s)
{
    ::dsn::lock_checker::check_wait_safety();
    ((::dsn::semaphore_provider*)(s))->wait();
}

DSN_API bool dsn_semaphore_wait_timeout(dsn_handle_t s, int timeout_milliseconds)
{
    return ((::dsn::semaphore_provider*)(s))->wait(timeout_milliseconds);
}

//------------------------------------------------------------------------------
//
// rpc
//
//------------------------------------------------------------------------------

// rpc calls
DSN_API dsn_address_t dsn_primary_address()
{
    return ::dsn::task::get_current_rpc()->primary_address().c_addr();
}

DSN_API bool dsn_rpc_register_handler(dsn_task_code_t code, const char* name, dsn_rpc_request_handler_t cb, void* param)
{
    ::dsn::rpc_handler_ptr h(new ::dsn::rpc_handler_info(code));
    h->name = std::string(name);
    h->c_handler = cb;
    h->parameter = param;

    return ::dsn::task::get_current_node()->rpc_register_handler(h);
}

DSN_API void* dsn_rpc_unregiser_handler(dsn_task_code_t code)
{
    auto h = ::dsn::task::get_current_node()->rpc_unregister_handler(code);
    return (h != nullptr) ? h->parameter : nullptr;
}

DSN_API dsn_task_t dsn_rpc_create_response_task(dsn_message_t request, dsn_rpc_response_handler_t cb, void* param, int reply_hash)
{
    auto msg = ((::dsn::message_ex*)request);
    return new ::dsn::rpc_response_task(msg, cb, param, reply_hash);
}

DSN_API void dsn_rpc_call(dsn_address_t server, dsn_task_t rpc_call, dsn_task_tracker_t tracker)
{
    ::dsn::rpc_response_task* task = (::dsn::rpc_response_task*)rpc_call;
    dassert(task->spec().type == TASK_TYPE_RPC_RESPONSE, "");
    task->set_tracker((dsn::task_tracker*)tracker);

    // TODO: remove this parameter in future
    auto msg = task->get_request();
    msg->server_address = server;
    ::dsn::task::get_current_rpc()->call(msg, task);
}

DSN_API dsn_message_t dsn_rpc_call_wait(dsn_address_t server, dsn_message_t request)
{
    auto msg = ((::dsn::message_ex*)request);
    msg->server_address = server;

    ::dsn::rpc_response_task* rtask = 
        new ::dsn::rpc_response_task(msg, nullptr, nullptr, 0);
    rtask->add_ref();
    ::dsn::task::get_current_rpc()->call(msg, rtask);
    rtask->wait();
    if (rtask->error() == ::dsn::ERR_OK)
    {
        auto msg = rtask->get_response();
        msg->add_ref(); // released by callers
        rtask->release_ref(); // added above
        return msg;
    }
    else
    {
        rtask->release_ref(); // added above
        return nullptr;
    }
}

DSN_API void dsn_rpc_call_one_way(dsn_address_t server, dsn_message_t request)
{
    auto msg = ((::dsn::message_ex*)request);
    msg->server_address = server;

    ::dsn::task::get_current_rpc()->call(msg, nullptr);
}

DSN_API void dsn_rpc_reply(dsn_message_t response)
{
    auto msg = ((::dsn::message_ex*)response);
    ::dsn::rpc_engine::reply(msg);
}

DSN_API void dsn_rpc_forward(dsn_message_t request, dsn_address_t addr)
{
    // TODO: enable real forwarding
    auto resp = dsn_msg_create_response(request);
    ::marshall(resp, addr);
    ::dsn::rpc_engine::reply((::dsn::message_ex*)resp, ::dsn::ERR_FORWARD_TO_OTHERS);
}

DSN_API dsn_message_t dsn_rpc_get_response(dsn_task_t rpc_call)
{
    ::dsn::rpc_response_task* task = (::dsn::rpc_response_task*)rpc_call;
    dassert(task->spec().type == TASK_TYPE_RPC_RESPONSE, "");
    auto msg = task->get_response();
    if (nullptr != msg)
    {
        msg->add_ref(); // released by callers
        return msg;
    }
    else
        return nullptr;
}

DSN_API void dsn_rpc_enqueue_response(dsn_task_t rpc_call, dsn_error_t err, dsn_message_t response)
{
    ::dsn::rpc_response_task* task = (::dsn::rpc_response_task*)rpc_call;
    dassert(task->spec().type == TASK_TYPE_RPC_RESPONSE, "");

    auto resp = ((::dsn::message_ex*)response);
    task->enqueue(err, resp);
}

//------------------------------------------------------------------------------
//
// file operations
//
//------------------------------------------------------------------------------
DSN_API dsn_handle_t dsn_file_open(const char* file_name, int flag, int pmode)
{
    return ::dsn::task::get_current_disk()->open(file_name, flag, pmode);
}

DSN_API dsn_error_t dsn_file_close(dsn_handle_t file)
{
    return ::dsn::task::get_current_disk()->close(file);
}

DSN_API dsn_task_t dsn_file_create_aio_task(dsn_task_code_t code, dsn_aio_handler_t cb, void* param, int hash)
{
    return new ::dsn::aio_task(code, cb, param, hash);
}

DSN_API void dsn_file_read(dsn_handle_t file, char* buffer, int count, uint64_t offset, dsn_task_t cb, dsn_task_tracker_t tracker)
{
    ::dsn::aio_task* callback((::dsn::aio_task*)cb);
    callback->set_tracker((dsn::task_tracker*)tracker);
    callback->aio()->buffer = buffer;
    callback->aio()->buffer_size = count;
    callback->aio()->engine = nullptr;
    callback->aio()->file = file;
    callback->aio()->file_offset = offset;
    callback->aio()->type = ::dsn::AIO_Read;

    ::dsn::task::get_current_disk()->read(callback);
}

DSN_API void dsn_file_write(dsn_handle_t file, const char* buffer, int count, uint64_t offset, dsn_task_t cb, dsn_task_tracker_t tracker)
{
    ::dsn::aio_task* callback((::dsn::aio_task*)cb);
    callback->set_tracker((dsn::task_tracker*)tracker);
    callback->aio()->buffer = (char*)buffer;
    callback->aio()->buffer_size = count;
    callback->aio()->engine = nullptr;
    callback->aio()->file = file;
    callback->aio()->file_offset = offset;
    callback->aio()->type = ::dsn::AIO_Write;

    ::dsn::task::get_current_disk()->write(callback);
}

DSN_API void dsn_file_copy_remote_directory(dsn_address_t remote, const char* source_dir, 
    const char* dest_dir, bool overwrite, dsn_task_t cb, dsn_task_tracker_t tracker)
{
    std::shared_ptr<::dsn::remote_copy_request> rci(new ::dsn::remote_copy_request());
    rci->source = remote;
    rci->source_dir = source_dir;
    rci->files.clear();
    rci->dest_dir = dest_dir;
    rci->overwrite = overwrite;

    ::dsn::aio_task* callback((::dsn::aio_task*)cb);
    callback->set_tracker((dsn::task_tracker*)tracker);

    ::dsn::task::get_current_nfs()->call(rci, callback);
}

DSN_API void dsn_file_copy_remote_files(dsn_address_t remote, const char* source_dir, const char** source_files, const char* dest_dir, bool overwrite, dsn_task_t cb, dsn_task_tracker_t tracker)
{
    std::shared_ptr<::dsn::remote_copy_request> rci(new ::dsn::remote_copy_request());
    rci->source = remote;
    rci->source_dir = source_dir;

    rci->files.clear();
    const char** p = source_files;
    while (*p != nullptr && **p != '\0')
    {
        rci->files.push_back(std::string(*p));
        p++;

        dinfo("copy remote file %s from %s", 
            *(p-1),
            rci->source.to_string()
            );
    }

    rci->dest_dir = dest_dir;
    rci->overwrite = overwrite;

    ::dsn::aio_task* callback((::dsn::aio_task*)cb);
    callback->set_tracker((dsn::task_tracker*)tracker);

    ::dsn::task::get_current_nfs()->call(rci, callback);
}

DSN_API size_t dsn_file_get_io_size(dsn_task_t cb_task)
{
    ::dsn::task* task = (::dsn::task*)cb_task;
    dassert(task->spec().type == TASK_TYPE_AIO, "");
    return ((::dsn::aio_task*)task)->get_transferred_size();
}

DSN_API void dsn_file_task_enqueue(dsn_task_t cb_task, dsn_error_t err, size_t size)
{
    ::dsn::task* task = (::dsn::task*)cb_task;
    dassert(task->spec().type == TASK_TYPE_AIO, "");

    ((::dsn::aio_task*)task)->enqueue(err, size);
}

//------------------------------------------------------------------------------
//
// zookeeper operations
//
//------------------------------------------------------------------------------
DSN_API dsn_zoo_visitor_t dsn_zoo_visitor(dsn_task_t zoo_tsk)
{
    return (dsn_zoo_visitor_t)(reinterpret_cast<dsn::zoo_task*>(zoo_tsk)->visitor());
}

DSN_API void dsn_zoo_fill_create_request(dsn_zoo_visitor_t visitor,
                                         const char* znode,
                                         int create_flags,
                                         const char* data,
                                         int data_length)
{
    dsn::zoo_visitor* v = reinterpret_cast<dsn::zoo_visitor*>(visitor);
    dassert(znode!=nullptr&&data_length>=0, "invalid parameter");

    v->_optype = dsn::ZOO_create;
    v->fill_znode_req(znode);
    v->fill_create(create_flags, data, data_length);
}

#define FILL_ZNODE(visitor, znode, optype) do{\
    dsn::zoo_visitor* v = reinterpret_cast<dsn::zoo_visitor*>(visitor);\
    dassert(znode != nullptr, "invalid parameter");\
    v->_optype = optype;\
    v->fill_znode_req(znode);\
}while(false)

DSN_API void dsn_zoo_fill_delete_request(dsn_zoo_visitor_t visitor,
                                         const char* znode)
{
    FILL_ZNODE(visitor, znode, dsn::ZOO_delete);
}

DSN_API void dsn_zoo_fill_set_request(dsn_zoo_visitor_t visitor, const char* znode, const char* data, int data_length)
{
    dsn::zoo_visitor* v = reinterpret_cast<dsn::zoo_visitor*>(visitor);
    dassert(znode != nullptr&&data_length>=0, "invalid parameter");

    v->_optype = dsn::ZOO_set;
    v->fill_znode_req(znode);
    v->fill_set(data, data_length);
}

DSN_API void dsn_zoo_fill_get_request(dsn_zoo_visitor_t visitor,
                                         const char* znode)
{
    FILL_ZNODE(visitor, znode, dsn::ZOO_get);
}

DSN_API void dsn_zoo_fill_get_children_request(dsn_zoo_visitor_t visitor, const char* znode)
{
    FILL_ZNODE(visitor, znode, dsn::ZOO_get_children);
}

DSN_API void dsn_zoo_fill_exist_request(dsn_zoo_visitor_t visitor,
                                        const char* znode)
{
    FILL_ZNODE(visitor, znode, dsn::ZOO_exist);
}

DSN_API void dsn_zoo_fill_add_watch_request(dsn_zoo_visitor_t visitor,
                                            const char* znode,
                                            bool is_node_watch)
{
    if (is_node_watch)
    {
        FILL_ZNODE(visitor, znode, dsn::ZOO_add_watch_for_node);
    }
    else
    {
        FILL_ZNODE(visitor, znode, dsn::ZOO_add_watch_for_dir);
    }
}

DSN_API dsn_handle_t dsn_zoo_connect(const char* zoo_hosts, int timeout_ms, dsn_task_t timeout_cb)
{
    dsn::task::get_current_zookeeper()->connect(zoo_hosts, timeout_ms, timeout_cb);
}

DSN_API void dsn_zoo_disconnect(dsn_handle_t zoo_handle)
{
    dsn::task::get_current_zookeeper()->disconnect(zoo_handle);
}

DSN_API dsn_handle_t dsn_zoo_create_task(dsn_task_code_t task_code,
                                         dsn_zoo_handler_t callback,
                                         void* param,
                                         int hash)
{
    return new dsn::zoo_task(task_code, callback, param, hash);
}

DSN_API dsn_error_t dsn_zoo_async_visit(dsn_handle_t zoo_handle, dsn_task_t zoo_tsk)
{
    return dsn::task::get_current_zookeeper()->async_visit(zoo_handle, zoo_tsk);
}
//------------------------------------------------------------------------------
//
// env
//
//------------------------------------------------------------------------------
DSN_API uint64_t dsn_now_ns()
{
    //return ::dsn::task::get_current_env()->now_ns();
    return ::dsn::service_engine::instance().env()->now_ns();
}

DSN_API uint64_t dsn_random64(uint64_t min, uint64_t max) // [min, max]
{
    //return ::dsn::task::get_current_env()->random64(min, max);
    return ::dsn::service_engine::instance().env()->random64(min, max);
}

//------------------------------------------------------------------------------
//
// system
//
//------------------------------------------------------------------------------
DSN_API bool dsn_register_app_role(const char* name, dsn_app_create create, dsn_app_start start, dsn_app_destroy destroy)
{
    auto& store = ::dsn::utils::singleton_store<std::string, ::dsn::service_app_role>::instance();
    ::dsn::service_app_role role;
    role.name = std::string(name);
    role.create = create;
    role.start = start;
    role.destroy = destroy;
    return store.put(role.name, role);
}

static bool run(const char* config_file, const char* config_arguments, bool sleep_after_init, std::string& app_name, int app_index);

DSN_API bool dsn_run_config(const char* config, bool sleep_after_init)
{
    std::string name;
    return run(config, nullptr, sleep_after_init, name, -1);
}

DSN_API void dsn_terminate()
{
# if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 0);
# else
    kill(getpid(), SIGKILL);
# endif
}

DSN_API bool dsn_mimic_app(const char* app_name, int index)
{
    auto worker = ::dsn::task::get_current_worker2();
    dassert(worker == nullptr, "cannot call dsn_mimic_app in rDSN threads");

    auto cnode = ::dsn::task::get_current_node2();
    if (cnode != nullptr)
    {
        const std::string& name = cnode->spec().name;
        if (name == std::string(app_name) ||
            (name.substr(0, strlen(app_name)) == std::string(app_name)
            && cnode->spec().index == index)
            )
        {
            return true;
        }
        else
        {
            derror("current thread is already attached to another rDSN app %s", name.c_str());
            return false;
        }
    }

    auto nodes = ::dsn::service_engine::instance().get_all_nodes();
    for (auto& n : nodes)
    {
        if (n.second->spec().name == std::string(app_name) ||
            (n.second->spec().name.substr(0, strlen(app_name)) == std::string(app_name)
            && n.second->spec().index == index)
            )
        {
            ::dsn::task::set_tls_dsn_context(n.second, nullptr, nullptr);
            return true;
        }
    }

    derror("cannot find host app %s with index %d", app_name, index);
    return false;
}

//
// run the system with arguments
//   config [-cargs k1=v1;k2=v2] [-app app_name] [-app_index index]
// e.g., config.ini -app replica -app_index 1 to start the first replica as a new process
//       config.ini -app replica to start ALL replicas (count specified in config) as a new process
//       config.ini -app replica -cargs replica-port=34556 to start ALL replicas with given port variable specified in config.ini
//       config.ini to start ALL apps as a new process
//
DSN_API void dsn_run(int argc, char** argv, bool sleep_after_init)
{
    if (argc < 2)
    {
        printf("invalid options for dsn_run\n"
            "// run the system with arguments\n"
            "//   config [-cargs k1=v1;k2=v2] [-app app_name] [-app_index index (1,2,3...)]\n"
            "// e.g., config.ini -app replica -app_index 1 to start the first replica as a new process\n"
            "//       config.ini -app replica to start ALL replicas (count specified in config) as a new process\n"
            "//       config.ini -app replica -cargs replica-port=34556 to start with %%replica-port%% var in config.ini\n"
            "//       config.ini to start ALL apps as a new process\n"
            );
        exit(1);
        return;
    }

    char* config = argv[1];
    std::string config_args = "";
    std::string app_name = "";
    int app_index = -1;

    for (int i = 2; i < argc;)
    {
        if (0 == strcmp(argv[i], "-cargs"))
        {
            if (++i < argc)
            {
                config_args = std::string(argv[i++]);
            }
        }

        else if (0 == strcmp(argv[i], "-app"))
        {
            if (++i < argc)
            {
                app_name = std::string(argv[i++]);
            }
        }

        else if (0 == strcmp(argv[i], "-app_index"))
        {
            if (++i < argc)
            {
                app_index = atoi(argv[i++]);
            }
        }
        else
        {
            printf("unknown arguments %s\n", argv[i]);
            exit(1);
            return;
        }
    }

    if (!run(config, config_args.size() > 0 ? config_args.c_str() : nullptr, sleep_after_init, app_name, app_index))
    {
        printf("run the system failed\n");
        dsn_terminate();
        return;
    }
}

namespace dsn {
    namespace tools
    {
        bool is_engine_ready()
        {
            return dsn_all.is_engine_ready();
        }

        tool_app* get_current_tool()
        {
            return dsn_all.tool;
        }
    }
}

extern void dsn_log_init();
bool run(const char* config_file, const char* config_arguments, bool sleep_after_init, std::string& app_name, int app_index)
{
    ::dsn::task::set_tls_dsn_context(nullptr, nullptr, nullptr);

    dsn_all.engine_ready = false;
    dsn_all.config_completed = false;
    dsn_all.tool = nullptr;
    dsn_all.engine = &::dsn::service_engine::instance();
    dsn_all.config.reset(new ::dsn::configuration());
    dsn_all.memory = nullptr;
    dsn_all.magic = 0xdeadbeef;

    if (!dsn_all.config->load(config_file, config_arguments))
    {
        printf("Fail to load config file %s\n", config_file);
        return false;
    }

    for (int i = 0; i <= dsn_task_code_max(); i++)
    {
        dsn_all.task_specs.push_back(::dsn::task_spec::get(i));
    }

    ::dsn::service_spec spec;
    spec.config = dsn_all.config;
    if (!spec.init())
    {
        printf("error in config file %s, exit ...\n", config_file);
        return false;
    }

    dsn_all.config_completed = true;

    // pause when necessary
    if (dsn_all.config->get_value<bool>("core", "pause_on_start", false,
        "whether to pause at startup time for easier debugging"))
    {
#if defined(_WIN32)
        printf("\nPause for debugging (pid = %d)...\n", static_cast<int>(::GetCurrentProcessId()));
#else
        printf("\nPause for debugging (pid = %d)...\n", static_cast<int>(getpid()));
#endif
        getchar();
    }

    // setup coredump
	auto& coredump_dir = spec.coredump_dir;
	dassert(!dsn::utils::filesystem::file_exists(coredump_dir), "%s should not be a file.", coredump_dir.c_str());
    if (!dsn::utils::filesystem::directory_exists(coredump_dir.c_str()))
    {
		if (!dsn::utils::filesystem::create_directory(coredump_dir))
		{
			dassert(false, "Fail to create %s.", coredump_dir.c_str());
		}
    }
	std::string cdir;
	if (!dsn::utils::filesystem::get_absolute_path(coredump_dir.c_str(), cdir))
	{
		dassert(false, "Fail to get absolute path from %s.", coredump_dir.c_str());
	}
    ::dsn::utils::coredump::init(cdir.c_str());

    // init tools
    dsn_all.tool = ::dsn::utils::factory_store<::dsn::tools::tool_app>::create(spec.tool.c_str(), 0, spec.tool.c_str());
    dsn_all.tool->install(spec);

    // init app specs
    if (!spec.init_app_specs())
    {
        printf("error in config file %s, exit ...\n", config_file);
        return false;
    }

    // init tool memory
    dsn_all.memory = ::dsn::utils::factory_store<::dsn::memory_provider>::create(
        spec.tools_memory_factory_name.c_str(), PROVIDER_TYPE_MAIN);

    // prepare minimum necessary
    ::dsn::service_engine::fast_instance().init_before_toollets(spec);

    // init logging
    dsn_log_init();

    // init toollets
    for (auto it = spec.toollets.begin(); it != spec.toollets.end(); it++)
    {
        auto tlet = dsn::tools::internal_use_only::get_toollet(it->c_str(), 0);
        dassert(tlet, "toolet not found");
        tlet->install(spec);
    }

    // init provider specific system inits
    dsn::tools::sys_init_before_app_created.execute(::dsn::service_engine::fast_instance().spec().config);

    // TODO: register sys_exit execution

    // init runtime
    ::dsn::service_engine::fast_instance().init_after_toollets();

    dsn_all.engine_ready = true;

    // init apps
    for (auto& sp : spec.app_specs)
    {
        if (!sp.run)
            continue;

        bool create_it = false;
        if (app_name == "") // create all apps
        {
            create_it = true;
        }
        else if (std::string("apps.") + app_name == sp.config_section)
        {
            if (app_index == -1)
                create_it = true;
            else
            {
                create_it = (app_index == sp.index);
            }
        }
        else
            create_it = false;

        if (create_it)
        {
            ::dsn::service_engine::fast_instance().start_node(sp);
        }
    }

    if (::dsn::service_engine::fast_instance().get_all_nodes().size() == 0)
    {
        printf("no app are created, usually because \n"
            "app_name is not specified correctly, should be 'xxx' in [apps.xxx]\n"
            "or app_index (1-based) is greater than specified count in config file\n"
            );
        exit(1);
    }
    
    // start cli if necessary
    if (dsn_all.config->get_value<bool>("core", "cli_local", true,
        "whether to enable local command line interface (cli)"))
    {
        ::dsn::command_manager::instance().start_local_cli();
    }

    if (dsn_all.config->get_value<bool>("core", "cli_remote", true,
        "whether to enable remote command line interface (using dsn.cli)"))
    {
        ::dsn::command_manager::instance().start_remote_cli();
    }

    // register local cli commands
    ::dsn::register_command("config-dump",
        "config-dump - dump configuration",
        "config-dump [to-this-config-file]",
        [](const std::vector<std::string>& args)
    {
        std::ostringstream oss;
        std::ofstream off;
        std::ostream* os = &oss;
        if (args.size() > 0)
        {
            off.open(args[0]);
            os = &off;

            oss << "config dump to file " << args[0] << std::endl;
        }

        dsn_all.config->dump(*os);
        return oss.str();
    });
    
    // invoke customized init after apps are created
    dsn::tools::sys_init_after_app_created.execute(::dsn::service_engine::fast_instance().spec().config);

    // start the tool
    dsn_all.tool->run();

    //
    if (sleep_after_init)
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }

    return true;
}

DSN_API int dsn_get_all_apps(dsn_app_info* info_buffer, int count)
{
    auto& as = ::dsn::service_engine::fast_instance().get_all_nodes();
    int i = 0;
    for (auto& kv : as)
    {
        if (i >= count)
            return (int)as.size();

        dsn_app_info& info = info_buffer[i++];
        info.app_context_ptr = kv.second->get_app_context_ptr();
        info.app_id = kv.second->id();
        strncpy(info.name, kv.second->spec().name.c_str(), sizeof(info.name));
        strncpy(info.type, kv.second->spec().type.c_str(), sizeof(info.type));
    }
    return i + 1;
}
