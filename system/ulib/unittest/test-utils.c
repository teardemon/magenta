// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>
#include <magenta/syscalls.h>
#include <runtime/status.h>
#include <unittest/test-utils.h>
#include <unittest/unittest.h>

#define TU_FAIL_ERRCODE 10

void* tu_malloc(size_t size)
{
    void* result = malloc(size);
    if (result == NULL) {
        // TODO(dje): printf may try to malloc too ...
        unittest_printf("out of memory trying to allocate %zu bytes\n", size);
        exit(TU_FAIL_ERRCODE);
    }
    return result;
}

char* tu_strdup(const char* s)
{
    size_t len = strlen(s) + 1;
    char* r = tu_malloc(len);
    strcpy(r, s);
    return r;
}

void tu_syscall_fail(const char *what, mx_status_t status)
{
    const char* reason = mx_strstatus(status);
    unittest_printf("syscall %s failed, rc %d (%s)\n", what, status, reason);
    exit(TU_FAIL_ERRCODE);
}

void tu_handle_close(mx_handle_t handle)
{
    mx_status_t status = mx_handle_close(handle);
    // TODO(dje): It's still an open question as to whether errors other than ERR_BAD_HANDLE are "advisory".
    if (status < 0) {
        tu_syscall_fail(__func__, handle);
    }
}

mx_handle_t tu_thread_create(tu_thread_start_func_t entry, void* arg,
                             const char* name)
{
    if (!name)
        name = "";
    mx_handle_t handle = mx_thread_create(entry, arg, name, strlen(name) + 1);
    if (handle < 0) {
        tu_syscall_fail(__func__, handle);
    }
    return handle;
}

static mx_status_t tu_wait(const mx_handle_t* handles, const mx_signals_t* signals,
                           uint32_t num_handles, uint32_t* result_index,
                           mx_time_t deadline,
                           mx_signals_state_t* signals_states)
{
    mx_status_t result;

    if (num_handles == 1u) {
        result =
            mx_handle_wait_one(*handles, *signals, deadline, signals_states);
    } else {
        result = mx_handle_wait_many(num_handles, handles, signals, deadline, NULL,
                                     signals_states);
    }

    // xyzdje, from mx_wait: TODO(cpu): implement |result_index|, see MG-33 bug.
    return result;
}

void tu_message_pipe_create(mx_handle_t* handle0, mx_handle_t* handle1)
{
    mx_handle_t handles[2];
    mx_status_t status = mx_message_pipe_create(handles, 0);
    if (status < 0)
        tu_syscall_fail(__func__, status);
    *handle0 = handles[0];
    *handle1 = handles[1];
}

void tu_message_write(mx_handle_t handle, const void* bytes, uint32_t num_bytes,
                      const mx_handle_t* handles, uint32_t num_handles, uint32_t flags)
{
    mx_status_t status = mx_message_write(handle, bytes, num_bytes, handles, num_handles, flags);
    if (status < 0)
        tu_syscall_fail(__func__, status);
}

void tu_message_read(mx_handle_t handle, void* bytes, uint32_t* num_bytes,
                     mx_handle_t* handles, uint32_t* num_handles, uint32_t flags)
{
    mx_status_t status = mx_message_read(handle, bytes, num_bytes, handles, num_handles, flags);
    if (status < 0)
        tu_syscall_fail(__func__, status);
}

// Wait until |handle| is readable or peer is closed.
// Result is true if readable, otherwise false.

bool tu_wait_readable(mx_handle_t handle)
{
    mx_signals_t signals = MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED;
    mx_signals_state_t signals_state;
    int64_t timeout = TU_WATCHDOG_DURATION_NANOSECONDS;
    mx_status_t result = tu_wait(&handle, &signals, 1, NULL, timeout, &signals_state);
    if (result != NO_ERROR)
        tu_syscall_fail(__func__, result);
    if ((signals_state.satisfied & MX_SIGNAL_READABLE) == 0) {
        unittest_printf("%s: peer closed\n", __func__);
        return false;
    }
    return true;
}

void tu_wait_signalled(mx_handle_t handle)
{
    mx_signals_t signals = MX_SIGNAL_SIGNALED;
    mx_signals_state_t signals_state;
    int64_t timeout = TU_WATCHDOG_DURATION_NANOSECONDS;
    mx_status_t result = tu_wait(&handle, &signals, 1, NULL, timeout, &signals_state);
    if (result != NO_ERROR)
        tu_syscall_fail(__func__, result);
    if ((signals_state.satisfied & MX_SIGNAL_SIGNALED) == 0) {
        unittest_printf("%s: unexpected return from tu_wait\n", __func__);
        exit(TU_FAIL_ERRCODE);
    }
}

int tu_process_get_return_code(mx_handle_t process)
{
    mx_process_info_t info;
    mx_ssize_t ret = mx_handle_get_info(process, MX_INFO_PROCESS, &info, sizeof(info));
    if (ret < 0)
        tu_syscall_fail("get process info", ret);
    if (ret != sizeof(info)) {
        // Bleah. Kernel/App mismatch?
        unittest_printf("%s: unexpected result from mx_handle_get_info\n", __func__);
        exit(TU_FAIL_ERRCODE);
    }
    return info.return_code;
}

int tu_process_wait_exit(mx_handle_t process)
{
    tu_wait_signalled(process);
    return tu_process_get_return_code(process);
}