/**
 * @file posixHelper.cc
 * @author Rafal Slota
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif // linux

#include "posixHelper.h"
#include "logging.h"
#include "monitoring/monitoring.h"

#include <boost/any.hpp>

#include <dirent.h>
#include <errno.h>
#include <fuse.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#if defined(__linux__)
#include <sys/fsuid.h>
#endif

#include <functional>
#include <map>
#include <set>
#include <string>

#if defined(__APPLE__)
/**
 * These funtions provide drop-in replacements for setfsuid and setfsgid on OSX
 */
static inline int setfsuid(uid_t uid)
{
    LOG_FCALL() << LOG_FARG(uid);

    uid_t olduid = geteuid();

    seteuid(uid);

    if (errno != EINVAL)
        errno = 0;

    return olduid;
}

static inline int setfsgid(gid_t gid)
{
    LOG_FCALL() << LOG_FARG(gid);

    gid_t oldgid = getegid();

    setegid(gid);

    if (errno != EINVAL)
        errno = 0;

    return oldgid;
}
#endif

namespace {
#if defined(__linux__) || defined(__APPLE__)
class UserCtxSetter {
public:
    UserCtxSetter(const uid_t uid, const gid_t gid)
        : m_uid{uid}
        , m_gid{gid}
        , m_prevUid{static_cast<uid_t>(setfsuid(uid))}
        , m_prevGid{static_cast<gid_t>(setfsgid(gid))}
        , m_currUid{static_cast<uid_t>(setfsuid(-1))}
        , m_currGid{static_cast<gid_t>(setfsgid(-1))}
    {
    }

    ~UserCtxSetter()
    {
        setfsuid(m_prevUid);
        setfsgid(m_prevGid);
    }

    bool valid() const
    {
        return (m_uid == static_cast<uid_t>(-1) || m_currUid == m_uid) &&
            (m_gid == static_cast<gid_t>(-1) || m_currGid == m_gid);
    }

private:
    const uid_t m_uid;
    const gid_t m_gid;
    const uid_t m_prevUid;
    const gid_t m_prevGid;
    const uid_t m_currUid;
    const gid_t m_currGid;
};
#else
struct UserCtxSetter {
public:
    UserCtxSetter(const int, const int) {}
    bool valid() const { return true; }
};
#endif

// Retry only in case one of these errors occured
const std::set<int> POSIX_RETRY_ERRORS = {EINTR, EIO, EAGAIN, EACCES, EBUSY,
    EMFILE, ETXTBSY, ESPIPE, EMLINK, EPIPE, EDEADLK, EWOULDBLOCK, ENOLINK,
    EADDRINUSE, EADDRNOTAVAIL, ENETDOWN, ENETUNREACH, ECONNABORTED, ECONNRESET,
    ENOTCONN, EHOSTUNREACH, ECANCELED, ESTALE
#if !defined(__APPLE__)
    ,
    ENONET, EHOSTDOWN, EREMOTEIO, ENOMEDIUM
#endif

};

inline bool POSIXRetryCondition(int result, const std::string &operation)
{
    auto ret = (result >= 0 ||
        POSIX_RETRY_ERRORS.find(errno) == POSIX_RETRY_ERRORS.end());

    if (!ret) {
        LOG(WARNING) << "Retrying POSIX helper operation '" << operation
                     << "' due to error: " << errno;
        ONE_METRIC_COUNTER_INC(
            "comp.helpers.mod.posix." + operation + ".retries");
    }

    return ret;
}

template <typename... Args1, typename... Args2>
inline folly::Future<folly::Unit> setResult(
    const std::string &operation, int (*fun)(Args2...), Args1 &&... args)
{
    auto ret =
        one::helpers::retry([&]() { return fun(std::forward<Args1>(args)...); },
            std::bind(POSIXRetryCondition, std::placeholders::_1, operation));

    if (ret < 0)
        return one::helpers::makeFuturePosixException(errno);

    return folly::makeFuture();
}

} // namespace

namespace one {
namespace helpers {

using namespace std::placeholders;

PosixFileHandle::PosixFileHandle(folly::fbstring fileId, const uid_t uid,
    const gid_t gid, const int fileHandle,
    std::shared_ptr<folly::Executor> executor, Timeout timeout)
    : FileHandle{std::move(fileId)}
    , m_uid{uid}
    , m_gid{gid}
    , m_fh{fileHandle}
    , m_executor{std::move(executor)}
    , m_timeout{std::move(timeout)}
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(uid) << LOG_FARG(gid)
                << LOG_FARG(fileHandle);
}

PosixFileHandle::~PosixFileHandle()
{
    LOG_FCALL();

    if (m_needsRelease.exchange(false)) {
        UserCtxSetter userCTX{m_uid, m_gid};
        if (!userCTX.valid()) {
            LOG(WARNING) << "Failed to release file " << m_fh
                         << ": failed to set user context";
            return;
        }

        if (close(m_fh) == -1) {
            auto ec = makePosixError(errno);
            LOG(WARNING) << "Failed to release file " << m_fh << ": "
                         << ec.message();
        }
    }
}

folly::Future<folly::IOBufQueue> PosixFileHandle::read(
    const off_t offset, const std::size_t size)
{
    LOG_FCALL() << LOG_FARG(offset) << LOG_FARG(size);

    auto timer = ONE_METRIC_TIMERCTX_CREATE("comp.helpers.mod.posix.read");

    return folly::via(m_executor.get(), [
        offset, size, uid = m_uid, gid = m_gid, fh = m_fh, fileId = m_fileId,
        timer = std::move(timer)
    ] {
        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<folly::IOBufQueue>(EDOM);

        folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};

        void *data = buf.preallocate(size, size).first;

        LOG_DBG(2) << "Attempting to read " << size << " bytes at offset "
                   << offset << " from file " << fileId;

        auto res = retry([&]() { return ::pread(fh, data, size, offset); },
            std::bind(POSIXRetryCondition, _1, "pread"));

        if (res == -1) {
            LOG_DBG(1) << "Reading from file " << fileId
                       << " failed with error " << errno;
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.errors.read");
            return makeFuturePosixException<folly::IOBufQueue>(errno);
        }

        buf.postallocate(res);

        LOG_DBG(2) << "Read " << res << " bytes from file " << fileId;

        ONE_METRIC_TIMERCTX_STOP(timer, res);

        return folly::makeFuture(std::move(buf));
    });
}

folly::Future<std::size_t> PosixFileHandle::write(
    const off_t offset, folly::IOBufQueue buf)
{
    LOG_FCALL() << LOG_FARG(offset) << LOG_FARG(buf.chainLength());

    auto timer = ONE_METRIC_TIMERCTX_CREATE("comp.helpers.mod.posix.write");
    return folly::via(m_executor.get(), [
        offset, buf = std::move(buf), uid = m_uid, gid = m_gid, fh = m_fh,
        fileId = m_fileId, timer = std::move(timer)
    ] {
        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<std::size_t>(EDOM);

        auto res = ::lseek(fh, offset, SEEK_SET);
        if (res == -1)
            return makeFuturePosixException<std::size_t>(errno);

        if (buf.empty())
            return folly::makeFuture<std::size_t>(0);

        auto iov = buf.front()->getIov();
        auto iov_size = iov.size();
        auto size = 0;

        LOG_DBG(2) << "Attempting to write " << buf.chainLength()
                   << " bytes at offset " << offset << " to file " << fileId;

        for (std::size_t iov_off = 0; iov_off < iov_size; iov_off += IOV_MAX) {
            res = retry(
                [&]() {
                    return ::writev(fh, iov.data() + iov_off,
                        std::min<std::size_t>(IOV_MAX, iov_size - iov_off));
                },
                [](int result) { return result != -1; });

            if (res == -1) {
                LOG_DBG(1) << "Writing to file " << fileId
                           << " failed with error " << errno;
                ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.errors.write");
                return makeFuturePosixException<std::size_t>(errno);
            }
            size += res;
        }

        LOG_DBG(2) << "Written " << size << " bytes to file " << fileId;

        ONE_METRIC_TIMERCTX_STOP(timer, size);

        return folly::makeFuture<std::size_t>(size);
    });
}

folly::Future<folly::Unit> PosixFileHandle::release()
{
    LOG_FCALL();

    if (!m_needsRelease.exchange(false))
        return folly::makeFuture();

    return folly::via(m_executor.get(),
        [ uid = m_uid, gid = m_gid, fh = m_fh, fileId = m_fileId ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.release");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            LOG_DBG(2) << "Closing file " << fileId;

            return setResult("close", close, fh);
        });
}

folly::Future<folly::Unit> PosixFileHandle::flush()
{
    LOG_FCALL();

    return folly::via(
        m_executor.get(), [ uid = m_uid, gid = m_gid, fileId = m_fileId ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.flush");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            LOG_DBG(2) << "Flushing file " << fileId;

            return folly::makeFuture();
        });
}

folly::Future<folly::Unit> PosixFileHandle::fsync(bool /*isDataSync*/)
{
    LOG_FCALL();

    return folly::via(m_executor.get(),
        [ uid = m_uid, gid = m_gid, fh = m_fh, fileId = m_fileId ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.fsync");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            LOG_DBG(2) << "Syncing file " << fileId;

            return setResult("fsync", ::fsync, fh);
        });
}

PosixHelper::PosixHelper(boost::filesystem::path mountPoint, const uid_t uid,
    const gid_t gid, std::shared_ptr<folly::Executor> executor, Timeout timeout)
    : m_mountPoint{std::move(mountPoint)}
    , m_uid{uid}
    , m_gid{gid}
    , m_executor{std::move(executor)}
    , m_timeout{std::move(timeout)}
{
    LOG_FCALL() << LOG_FARG(mountPoint) << LOG_FARG(uid) << LOG_FARG(gid);
}

folly::Future<struct stat> PosixHelper::getattr(const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(fileId);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.getattr");

            struct stat stbuf = {};

            LOG_DBG(2) << "Attempting to stat file " << filePath;

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException<struct stat>(EDOM);

            auto res =
                retry([&]() { return ::lstat(filePath.c_str(), &stbuf); },
                    std::bind(POSIXRetryCondition, _1, "lstat"));

            if (res == -1) {
                LOG_DBG(1) << "Stating file " << filePath
                           << " failed with error " << errno;
                return makeFuturePosixException<struct stat>(errno);
            }

            return folly::makeFuture(stbuf);
        });
}

folly::Future<folly::Unit> PosixHelper::access(
    const folly::fbstring &fileId, const int mask)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(mask);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mask, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.access");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            LOG_DBG(2) << "Attempting to access file " << filePath;

            return setResult("access", ::access, filePath.c_str(), mask);
        });
}

folly::Future<folly::fbvector<folly::fbstring>> PosixHelper::readdir(
    const folly::fbstring &fileId, off_t offset, size_t count)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(offset) << LOG_FARG(count);

    return folly::via(m_executor.get(), [
        filePath = root(fileId), offset, count, uid = m_uid, gid = m_gid
    ] {
        ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.readdir");

        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                EDOM);

        folly::fbvector<folly::fbstring> ret;

        LOG_DBG(2) << "Attempting to read directory " << filePath;

        DIR *dir;
        struct dirent *dp;
        dir = retry([&]() { return opendir(filePath.c_str()); },
            [](DIR *d) {
                return d != nullptr ||
                    POSIX_RETRY_ERRORS.find(errno) == POSIX_RETRY_ERRORS.end();
            });

        if (!dir) {
            LOG_DBG(1) << "Opening directory " << filePath
                       << " failed with error " << errno;
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                errno);
        }

        int offset_ = offset, count_ = count;
        while ((dp = retry([&]() { return ::readdir(dir); },
                    [](struct dirent *de) {
                        return de != nullptr ||
                            POSIX_RETRY_ERRORS.find(errno) ==
                            POSIX_RETRY_ERRORS.end();
                    })) != nullptr &&
            count_ > 0) {
            if (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")) {
                if (offset_ > 0) {
                    --offset_;
                }
                else {
                    ret.push_back(folly::fbstring(dp->d_name));
                    --count_;
                }
            }
        }
        closedir(dir);

        LOG_DBG(2) << "Read directory " << filePath << " at offset " << offset
                   << " with entries " << LOG_VEC(ret);

        return folly::makeFuture<folly::fbvector<folly::fbstring>>(
            std::move(ret));
    });
}

folly::Future<folly::fbstring> PosixHelper::readlink(
    const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(fileId);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.readlink");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException<folly::fbstring>(EDOM);

            LOG_DBG(1) << "Attempting to read link " << filePath;

            constexpr std::size_t maxSize = 1024;
            auto buf = folly::IOBuf::create(maxSize);
            auto res = retry(
                [&]() {
                    return ::readlink(filePath.c_str(),
                        reinterpret_cast<char *>(buf->writableData()),
                        maxSize - 1);
                },
                std::bind(POSIXRetryCondition, _1, "readlink"));

            if (res == -1) {
                LOG_DBG(1) << "Reading link " << filePath
                           << " failed with error " << errno;
                return makeFuturePosixException<folly::fbstring>(errno);
            }

            buf->append(res);

            auto target = buf->moveToFbString();

            LOG_DBG(2) << "Read link " << filePath << " - resolves to "
                       << target;

            return folly::makeFuture(std::move(target));
        });
}

folly::Future<folly::Unit> PosixHelper::mknod(const folly::fbstring &fileId,
    const mode_t unmaskedMode, const FlagsSet &flags, const dev_t rdev)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(unmaskedMode)
                << LOG_FARG(flagsToMask(flags));

    const mode_t mode = unmaskedMode | flagsToMask(flags);
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, rdev, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.mknod");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            int res;

            /* On Linux this could just be 'mknod(path, mode, rdev)' but this
               is more portable */
            if (S_ISREG(mode)) {
                res = retry(
                    [&]() {
                        return ::open(filePath.c_str(),
                            O_CREAT | O_EXCL | O_WRONLY, mode);
                    },
                    std::bind(POSIXRetryCondition, _1, "open"));

                if (res >= 0)
                    res = close(res);
            }
            else if (S_ISFIFO(mode)) {
                res = retry([&]() { return ::mkfifo(filePath.c_str(), mode); },
                    std::bind(POSIXRetryCondition, _1, "mkfifo"));
            }
            else {
                res = retry(
                    [&]() { return ::mknod(filePath.c_str(), mode, rdev); },
                    std::bind(POSIXRetryCondition, _1, "mknod"));
            }

            if (res == -1)
                return makeFuturePosixException(errno);

            return folly::makeFuture();
        });
}

folly::Future<folly::Unit> PosixHelper::mkdir(
    const folly::fbstring &fileId, const mode_t mode)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(mode);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.mkdir");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("mkdir", ::mkdir, filePath.c_str(), mode);
        });
}

folly::Future<folly::Unit> PosixHelper::unlink(
    const folly::fbstring &fileId, const size_t /*currentSize*/)
{
    LOG_FCALL() << LOG_FARG(fileId);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.unlink");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("unlink", ::unlink, filePath.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::rmdir(const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(fileId);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.rmdir");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("rmdir", ::rmdir, filePath.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::symlink(
    const folly::fbstring &from, const folly::fbstring &to)
{
    LOG_FCALL() << LOG_FARG(from) << LOG_FARG(to);

    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.symlink");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("symlink", ::symlink, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::rename(
    const folly::fbstring &from, const folly::fbstring &to)
{
    LOG_FCALL() << LOG_FARG(from) << LOG_FARG(to);

    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.rename");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("rename", ::rename, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::link(
    const folly::fbstring &from, const folly::fbstring &to)
{
    LOG_FCALL() << LOG_FARG(from) << LOG_FARG(to);

    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.link");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("link", ::link, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::chmod(
    const folly::fbstring &fileId, const mode_t mode)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(mode);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.chmod");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("chmod", ::chmod, filePath.c_str(), mode);
        });
}

folly::Future<folly::Unit> PosixHelper::chown(
    const folly::fbstring &fileId, const uid_t uid, const gid_t gid)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(uid) << LOG_FARG(gid);

    return folly::via(m_executor.get(), [
        filePath = root(fileId), argUid = uid, argGid = gid, uid = m_uid,
        gid = m_gid
    ] {
        ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.chown");

        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException(EDOM);

        return setResult("chown", ::chown, filePath.c_str(), argUid, argGid);
    });
}

folly::Future<folly::Unit> PosixHelper::truncate(const folly::fbstring &fileId,
    const off_t size, const size_t /*currentSize*/)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(size);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), size, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.truncate");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult("truncate", ::truncate, filePath.c_str(), size);
        });
}

folly::Future<FileHandlePtr> PosixHelper::open(
    const folly::fbstring &fileId, const int flags, const Params &)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(flags);

    return folly::via(m_executor.get(), [
        fileId, filePath = root(fileId), flags, executor = m_executor,
        uid = m_uid, gid = m_gid, timeout = m_timeout
    ]() mutable {
        ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.open");

        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<FileHandlePtr>(EDOM);

        int res = retry([&]() { return ::open(filePath.c_str(), flags); },
            std::bind(POSIXRetryCondition, _1, "open"));

        if (res == -1)
            return makeFuturePosixException<FileHandlePtr>(errno);

        auto handle = std::make_shared<PosixFileHandle>(std::move(fileId), uid,
            gid, res, std::move(executor), std::move(timeout));

        return folly::makeFuture<FileHandlePtr>(std::move(handle));
    });
}

folly::Future<folly::fbstring> PosixHelper::getxattr(
    const folly::fbstring &fileId, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(name);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), name, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.getxattr");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException<folly::fbstring>(EDOM);

            constexpr std::size_t initialMaxSize = 256;
            auto buf = folly::IOBuf::create(initialMaxSize);

            int res = retry(
                [&]() {
                    return ::getxattr(filePath.c_str(), name.c_str(),
                        reinterpret_cast<char *>(buf->writableData()),
                        initialMaxSize - 1
#if defined(__APPLE__)
                        ,
                        0, 0
#endif
                    );
                },
                std::bind(POSIXRetryCondition, _1, "getxattr"));
            // If the initial buffer for xattr value was too small, try again
            // with maximum allowed value
            if (res == -1 && errno == ERANGE) {
                buf = folly::IOBuf::create(XATTR_SIZE_MAX);
                res = retry(
                    [&]() {
                        return ::getxattr(filePath.c_str(), name.c_str(),
                            reinterpret_cast<char *>(buf->writableData()),
                            XATTR_SIZE_MAX - 1
#if defined(__APPLE__)
                            ,
                            0, 0
#endif
                        );
                    },
                    std::bind(POSIXRetryCondition, _1, "getxattr"));
            }

            if (res == -1)
                return makeFuturePosixException<folly::fbstring>(errno);

            buf->append(res);
            return folly::makeFuture(buf->moveToFbString());
        });
}

folly::Future<folly::Unit> PosixHelper::setxattr(const folly::fbstring &fileId,
    const folly::fbstring &name, const folly::fbstring &value, bool create,
    bool replace)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(name) << LOG_FARG(value)
                << LOG_FARG(create) << LOG_FARG(replace);

    return folly::via(m_executor.get(), [
        filePath = root(fileId), name, value, create, replace, uid = m_uid,
        gid = m_gid
    ] {
        ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.setxattr");

        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<folly::Unit>(EDOM);

        int flags = 0;

        if (create && replace) {
            return makeFuturePosixException<folly::Unit>(EINVAL);
        }

        if (create) {
            flags = XATTR_CREATE;
        }
        else if (replace) {
            flags = XATTR_REPLACE;
        }

        return setResult("setxattr", ::setxattr, filePath.c_str(), name.c_str(),
            value.c_str(), value.size(),
#if defined(__APPLE__)
            0,
#endif
            flags);
    });
}

folly::Future<folly::Unit> PosixHelper::removexattr(
    const folly::fbstring &fileId, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(fileId) << LOG_FARG(name);

    return folly::via(m_executor.get(),
        [ filePath = root(fileId), name, uid = m_uid, gid = m_gid ] {
            ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.removexattr");

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(
                "removexattr", ::removexattr, filePath.c_str(), name.c_str()
#if defined(__APPLE__)
                                                                    ,
                0
#endif
            );
        });
}

folly::Future<folly::fbvector<folly::fbstring>> PosixHelper::listxattr(
    const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(fileId);

    return folly::via(m_executor.get(), [
        filePath = root(fileId), uid = m_uid, gid = m_gid
    ] {
        ONE_METRIC_COUNTER_INC("comp.helpers.mod.posix.listxattr");

        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                EDOM);

        folly::fbvector<folly::fbstring> ret;

        ssize_t buflen = retry(
            [&]() {
                return ::listxattr(filePath.c_str(), NULL, 0
#if defined(__APPLE__)
                    ,
                    0
#endif
                );
            },
            std::bind(POSIXRetryCondition, _1, "listxattr"));

        if (buflen == -1)
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                errno);

        if (buflen == 0)
            return folly::makeFuture<folly::fbvector<folly::fbstring>>(
                std::move(ret));

        auto buf = std::unique_ptr<char[]>(new char[buflen]);
        buflen = ::listxattr(filePath.c_str(), buf.get(), buflen
#if defined(__APPLE__)
            ,
            0
#endif
        );

        if (buflen == -1)
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                errno);

        char *xattrNamePtr = buf.get();
        while (xattrNamePtr < buf.get() + buflen) {
            ret.emplace_back(xattrNamePtr);
            xattrNamePtr +=
                strnlen(xattrNamePtr, buflen - (buf.get() - xattrNamePtr)) + 1;
        }

        return folly::makeFuture<folly::fbvector<folly::fbstring>>(
            std::move(ret));
    });
}

} // namespace helpers
} // namespace one
