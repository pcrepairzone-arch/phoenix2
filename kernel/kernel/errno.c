/*
 * errno.c – Error code implementation for RISC OS Phoenix
 * Author: Error Handling Framework – 15 Feb 2026
 */

#include "errno.h"

/* Thread-local errno variable */
__thread int errno = 0;

/* Error message table */
static const char *error_messages[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "I/O error",
    [ENXIO] = "No such device or address",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Out of memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [ENOTBLK] = "Block device required",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [ENFILE] = "File table overflow",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [ETXTBSY] = "Text file busy",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EMLINK] = "Too many links",
    [EPIPE] = "Broken pipe",
    [EDOM] = "Math argument out of domain",
    [ERANGE] = "Math result not representable",
    [EDEADLK] = "Resource deadlock would occur",
    [ENAMETOOLONG] = "File name too long",
    [ENOLCK] = "No record locks available",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many symbolic links encountered",
    [ENOMSG] = "No message of desired type",
    [EIDRM] = "Identifier removed",
    [ENOSTR] = "Device not a stream",
    [ENODATA] = "No data available",
    [ETIME] = "Timer expired",
    [EPROTO] = "Protocol error",
    [EOVERFLOW] = "Value too large for defined data type",
    [EBADMSG] = "Not a data message",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTDOWN] = "Host is down",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress",
    [EINPROGRESS] = "Operation now in progress",
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

char *strerror(int errnum)
{
    if (errnum < 0 || errnum >= ARRAY_SIZE(error_messages) || !error_messages[errnum]) {
        return "Unknown error";
    }
    return (char *)error_messages[errnum];
}
