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
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <magenta/processargs.h>
#include <magenta/syscalls.h>

#include <mxio/debug.h>
#include <mxio/io.h>
#include <mxio/remoteio.h>
#include <mxio/util.h>
#include <mxio/vfs.h>

#define MXDEBUG 0

// non-thread-safe emulation of unistd io functions
// using the mxio transports

#define MAX_MXIO_FD 256

static mxio_t* mxio_root_handle = NULL;

static mx_handle_t mxio_process_handle = 0;

static mxio_t* mxio_fdtab[MAX_MXIO_FD] = {
    NULL,
};

void mxio_install_root(mxio_t* root) {
    if (mxio_root_handle == NULL) {
        mxio_root_handle = root;
    }
}

//TODO: fd's pointing to same mxio, refcount, etc

int mxio_bind_to_fd(mxio_t* io, int fd) {
    if (fd >= 0) {
        if (fd >= MAX_MXIO_FD)
            return ERR_INVALID_ARGS;
        if (mxio_fdtab[fd])
            return ERR_ALREADY_EXISTS;
        mxio_fdtab[fd] = io;
        return fd;
    }
    for (fd = 0; fd < MAX_MXIO_FD; fd++) {
        if (mxio_fdtab[fd] == NULL) {
            mxio_fdtab[fd] = io;
            return fd;
        }
    }
    return ERR_NO_RESOURCES;
}

static inline mxio_t* fd_to_io(int fd) {
    if ((fd < 0) || (fd >= MAX_MXIO_FD))
        return NULL;
    return mxio_fdtab[fd];
}

static void mxio_exit(void) {
    int fd;
    for (fd = 0; fd < MAX_MXIO_FD; fd++) {
        mxio_t* io = mxio_fdtab[fd];
        if (io) {
            io->ops->close(io);
            mxio_fdtab[fd] = NULL;
        }
    }
}

// hook into musl FILE* io
#if WITH_LIBC_IO_HOOKS
#define __open __libc_io_open
#define __close __libc_io_close
#else
#define __open open
#define __close close
#endif

ssize_t __libc_io_write(int fd, const void* data, size_t len) {
    return write(fd, data, len);
}

int __libc_io_readv(int fd, const struct iovec* iov, int num) {
    ssize_t count = 0;
    ssize_t r;
    while (num > 0) {
        if (iov->iov_len != 0) {
            r = read(fd, iov->iov_base, iov->iov_len);
            if (r < 0) {
                return count ? count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return count + r;
            }
            count += r;
        }
        iov++;
        num--;
    }
    return count;
}

int __libc_io_writev(int fd, const struct iovec* iov, int num) {
    ssize_t count = 0;
    ssize_t r;
    while (num > 0) {
        if (iov->iov_len != 0) {
            r = write(fd, iov->iov_base, iov->iov_len);
            if (r < 0) {
                return count ? count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return count + r;
            }
            count += r;
        }
        iov++;
        num--;
    }
    return count;
}

mx_handle_t mxio_get_process_handle(void) {
    return mxio_process_handle;
}

// hook into libc process startup
void __libc_extensions_init(mx_proc_info_t* pi) {
    int n;
    // extract handles we care about
    for (n = 0; n < pi->handle_count; n++) {
        unsigned arg = MX_HND_INFO_ARG(pi->handle_info[n]);
        mx_handle_t h = pi->handle[n];

        switch (MX_HND_INFO_TYPE(pi->handle_info[n])) {
        case MX_HND_TYPE_MXIO_ROOT:
            mxio_root_handle = mxio_remote_create(h, 0);
            break;
        case MX_HND_TYPE_MXIO_REMOTE:
            // remote objects may have a second handle
            // which is for signalling events
            if (((n + 1) < pi->handle_count) &&
                (pi->handle_info[n] == pi->handle_info[n + 1])) {
                mxio_fdtab[arg] = mxio_remote_create(h, pi->handle[n + 1]);
                pi->handle_info[n + 1] = 0;
            } else {
                mxio_fdtab[arg] = mxio_remote_create(h, 0);
            }
            break;
        case MX_HND_TYPE_MXIO_PIPE:
            mxio_fdtab[arg] = mxio_pipe_create(h);
            break;
        case MX_HND_TYPE_PROC_SELF:
            mxio_process_handle = h;
            continue;
        default:
            // unknown handle, leave it alone
            continue;
        }
        pi->handle[n] = 0;
        pi->handle_info[n] = 0;
    }

    // install null stdin/out/err if not init'd
    for (n = 0; n < 3; n++) {
        if (mxio_fdtab[n] == NULL) {
            mxio_fdtab[n] = mxio_null_create();
        }
    }

    atexit(mxio_exit);
}

mx_status_t mxio_clone_root(mx_handle_t* handles, uint32_t* types) {
    // TODO: better solution
    mx_status_t r = mxio_root_handle->ops->clone(mxio_root_handle, handles, types);
    if (r > 0) {
        *types = MX_HND_TYPE_MXIO_ROOT;
    }
    return r;
}

mx_status_t mxio_clone_fd(int fd, int newfd, mx_handle_t* handles, uint32_t* types) {
    mx_status_t r;
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }
    if ((r = io->ops->clone(io, handles, types)) > 0) {
        for (int i = 0; i < r; i++) {
            types[i] |= (newfd << 16);
        }
    }
    return r;
}

ssize_t mxio_ioctl(int fd, int op, const void* in_buf, size_t in_len, void* out_buf, size_t out_len) {
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }
    if (!io->ops->ioctl) {
        return ERR_NOT_SUPPORTED;
    }
    return io->ops->ioctl(io, op, in_buf, in_len, out_buf, out_len);
}

mx_status_t mxio_create_subprocess_handles(mx_handle_t* handles, uint32_t* types, size_t count) {
    mx_status_t r;
    size_t n = 0;

    if (count < MXIO_MAX_HANDLES)
        return ERR_NO_MEMORY;

    if ((r = mxio_clone_root(handles + n, types + n)) < 0) {
        return r;
    }
    n += r;
    count -= r;

    for (int fd = 0; (fd < MAX_MXIO_FD) && (count >= MXIO_MAX_HANDLES); fd++) {
        if ((r = mxio_clone_fd(fd, fd, handles + n, types + n)) <= 0) {
            continue;
        }
        n += r;
        count -= r;
    }
    return n;
}

mx_handle_t mxio_start_process(int args_count, char* args[]) {
    // worset case slots for all fds plus root handle
    // plus a process handle possibly added by start process
    mx_handle_t hnd[(2 + MAX_MXIO_FD) * MXIO_MAX_HANDLES];
    uint32_t ids[(2 + MAX_MXIO_FD) + MXIO_MAX_HANDLES];
    mx_status_t r;

    r = mxio_create_subprocess_handles(hnd, ids, (1 + MAX_MXIO_FD) * MXIO_MAX_HANDLES);
    if (r < 0) {
        return r;
    } else {
        return mxio_start_process_etc(args_count, args, r, hnd, ids);
    }
}

mx_status_t mxio_wait_fd(int fd, uint32_t events, uint32_t* pending) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    return io->ops->wait(io, events, pending, MX_TIME_INFINITE);
}

//TODO: errors -> errno
ssize_t read(int fd, void* buf, size_t count) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    if (buf == NULL)
        return ERR_INVALID_ARGS;
    return io->ops->read(io, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    if (buf == NULL)
        return ERR_INVALID_ARGS;
    return io->ops->write(io, buf, count);
}

int __close(int fd) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    int r = io->ops->close(io);
    mxio_fdtab[fd] = NULL;
    return r;
}

off_t lseek(int fd, off_t offset, int whence) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    return io->ops->seek(io, offset, whence);
}

int getdirents(int fd, void* ptr, size_t len) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    return io->ops->misc(io, MX_RIO_READDIR, len, ptr, 0);
}

int __open(const char* path, int flags, ...) {
    mxio_t* io = NULL;
    mx_status_t r;
    int fd;
    if (path == NULL) {
        return ERR_INVALID_ARGS;
    }
    if (mxio_root_handle == NULL) {
        return ERR_BAD_HANDLE;
    }
    r = mxio_root_handle->ops->open(mxio_root_handle, path, flags, &io);
    if (r < 0)
        return r;
    if (io == NULL)
        return ERR_IO;
    fd = mxio_bind_to_fd(io, -1);
    if (fd < 0) {
        io->ops->close(io);
        return ERR_NO_RESOURCES;
    }
    return fd;
}

int mx_stat(mxio_t* io, struct stat* s) {
    vnattr_t attr;
    int r = io->ops->misc(io, MX_RIO_STAT, sizeof(attr), &attr, 0);
    if (r < 0)
        return r;
    if (r < (int)sizeof(attr))
        return ERR_IO;
    memset(s, 0, sizeof(struct stat));
    s->st_mode = attr.mode;
    s->st_size = attr.size;
    s->st_ino = attr.inode;
    return 0;
}

int fstat(int fd, struct stat* s) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL)
        return ERR_BAD_HANDLE;
    return mx_stat(io, s);
}

int stat(const char* fn, struct stat* s) {
    mxio_t* io;
    mx_status_t r;
    if (fn == NULL) {
        return ERR_INVALID_ARGS;
    }
    if (mxio_root_handle == NULL) {
        return ERR_BAD_HANDLE;
    }
    if ((r = mx_open(mxio_root_handle, fn, 0, &io)) < 0) {
        return r;
    }
    r = mx_stat(io, s);
    mx_close(io);
    return r;
}

int pipe(int pipefd[2]) {
    mxio_t *a, *b;
    int r = mxio_pipe_pair(&a, &b);
    if (r < 0)
        return r;
    pipefd[0] = mxio_bind_to_fd(a, -1);
    if (pipefd[0] < 0) {
        mx_close(a);
        mx_close(b);
        return pipefd[0];
    }
    pipefd[1] = mxio_bind_to_fd(b, -1);
    if (pipefd[1] < 0) {
        close(pipefd[0]);
        mx_close(b);
        return pipefd[1];
    }
    return 0;
}