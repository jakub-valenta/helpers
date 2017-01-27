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

#include <boost/any.hpp>

#include <dirent.h>
#include <errno.h>
#include <fuse.h>
#include <sys/stat.h>

#if defined(__linux__)
#include <sys/fsuid.h>
#endif

#include <map>
#include <string>

#if defined(__APPLE__)
/**
 * These funtions provide drop-in replacements for setfsuid and setfsgid on OSX
 */
static __inline int setfsuid(uid_t uid)
{
    uid_t olduid = geteuid();

    seteuid(uid);

    if (errno != EINVAL)
        errno = 0;

    return olduid;
}

static __inline int setfsgid(gid_t gid)
{
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

template <typename... Args1, typename... Args2>
inline folly::Future<folly::Unit> setResult(
    int (*fun)(Args2...), Args1 &&... args)
{
    if (fun(std::forward<Args1>(args)...) < 0)
        return one::helpers::makeFuturePosixException(errno);

    return folly::makeFuture();
}

} // namespace

namespace one {
namespace helpers {

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
}

PosixFileHandle::~PosixFileHandle()
{
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
    return folly::via(m_executor.get(), [=] {
        try {
            return folly::makeFuture(readSync(offset, size));
        }
        catch (const std::system_error &e) {
            return makeFuturePosixException<folly::IOBufQueue>(
                e.code().value());
        }
    });
}

folly::IOBufQueue PosixFileHandle::readSync(
    const off_t offset, const std::size_t size)
{
    UserCtxSetter userCTX{m_uid, m_gid};
    if (!userCTX.valid())
        throw makePosixException(EDOM);

    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    void *data = buf.preallocate(size, size).first;

    auto res = ::pread(m_fh, data, size, offset);

    if (res == -1)
        throw makePosixException(errno);

    buf.postallocate(res);
    return buf;
}

folly::Future<std::size_t> PosixFileHandle::write(
    const off_t offset, folly::IOBufQueue buf)
{
    return folly::via(m_executor.get(), [ =, buf = std::move(buf) ]() mutable {
        try {
            return folly::makeFuture(writeSync(offset, std::move(buf)));
        }
        catch (const std::system_error &e) {
            return makeFuturePosixException<std::size_t>(e.code().value());
        }
    });
}

std::size_t PosixFileHandle::writeSync(
    const off_t offset, folly::IOBufQueue buf)
{
    UserCtxSetter userCTX{m_uid, m_gid};
    if (!userCTX.valid())
        throw makePosixException(EDOM);

    auto res = ::lseek(m_fh, offset, SEEK_SET);
    if (res == -1)
        throw makePosixException(errno);

    if (buf.empty())
        return 0;

    auto iov = buf.front()->getIov();
    auto iov_size = iov.size();
    auto size = 0;

    for (std::size_t iov_off = 0; iov_off < iov_size; iov_off += IOV_MAX) {
        res = ::writev(m_fh, iov.data() + iov_off,
            std::min<std::size_t>(IOV_MAX, iov_size - iov_off));
        if (res == -1)
            throw makePosixException(errno);
        size += res;
    }

    return size;
}

folly::Future<folly::Unit> PosixFileHandle::release()
{
    if (!m_needsRelease.exchange(false))
        return folly::makeFuture();

    return folly::via(
        m_executor.get(), [ uid = m_uid, gid = m_gid, fh = m_fh ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(close, fh);
        });
}

folly::Future<folly::Unit> PosixFileHandle::flush()
{
    return folly::via(
        m_executor.get(), [ uid = m_uid, gid = m_gid, fh = m_fh ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return folly::makeFuture();
        });
}

folly::Future<folly::Unit> PosixFileHandle::fsync(bool /*isDataSync*/)
{
    return folly::via(
        m_executor.get(), [ uid = m_uid, gid = m_gid, fh = m_fh ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::fsync, fh);
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
}

folly::Future<struct stat> PosixHelper::getattr(const folly::fbstring &fileId)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            struct stat stbuf = {};

            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException<struct stat>(EDOM);

            if (::lstat(filePath.c_str(), &stbuf) == -1)
                return makeFuturePosixException<struct stat>(errno);

            return folly::makeFuture(stbuf);
        });
}

folly::Future<folly::Unit> PosixHelper::access(
    const folly::fbstring &fileId, const int mask)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mask, uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::access, filePath.c_str(), mask);
        });
}

folly::Future<folly::fbvector<folly::fbstring>> PosixHelper::readdir(
    const folly::fbstring &fileId, off_t offset, size_t count)
{
    return folly::via(m_executor.get(), [
        filePath = root(fileId), offset, count, uid = m_uid, gid = m_gid
    ] {
        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                EDOM);

        folly::fbvector<folly::fbstring> ret;

        DIR *dir;
        struct dirent *dp;
        dir = opendir(filePath.c_str());

        if (!dir)
            return makeFuturePosixException<folly::fbvector<folly::fbstring>>(
                errno);

        int offset_ = offset, count_ = count;
        while ((dp = ::readdir(dir)) != NULL && count_ > 0) {
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

        return folly::makeFuture<folly::fbvector<folly::fbstring>>(
            std::move(ret));
    });
}

folly::Future<folly::fbstring> PosixHelper::readlink(
    const folly::fbstring &fileId)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException<folly::fbstring>(EDOM);

            constexpr std::size_t maxSize = 1024;
            auto buf = folly::IOBuf::create(maxSize);
            const int res = ::readlink(filePath.c_str(),
                reinterpret_cast<char *>(buf->writableData()), maxSize - 1);

            if (res == -1)
                return makeFuturePosixException<folly::fbstring>(errno);

            buf->append(res);
            return folly::makeFuture(buf->moveToFbString());
        });
}

folly::Future<folly::Unit> PosixHelper::mknod(const folly::fbstring &fileId,
    const mode_t unmaskedMode, const FlagsSet &flags, const dev_t rdev)
{
    const mode_t mode = unmaskedMode | flagsToMask(flags);
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, rdev, uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            int res;

            /* On Linux this could just be 'mknod(path, mode, rdev)' but this
               is more portable */
            if (S_ISREG(mode)) {
                res =
                    ::open(filePath.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
                if (res >= 0)
                    res = close(res);
            }
            else if (S_ISFIFO(mode)) {
                res = ::mkfifo(filePath.c_str(), mode);
            }
            else {
                res = ::mknod(filePath.c_str(), mode, rdev);
            }

            if (res == -1)
                return makeFuturePosixException(errno);

            return folly::makeFuture();
        });
}

folly::Future<folly::Unit> PosixHelper::mkdir(
    const folly::fbstring &fileId, const mode_t mode)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::mkdir, filePath.c_str(), mode);
        });
}

folly::Future<folly::Unit> PosixHelper::unlink(const folly::fbstring &fileId)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::unlink, filePath.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::rmdir(const folly::fbstring &fileId)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::rmdir, filePath.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::symlink(
    const folly::fbstring &from, const folly::fbstring &to)
{
    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::symlink, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::rename(
    const folly::fbstring &from, const folly::fbstring &to)
{
    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::rename, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::link(
    const folly::fbstring &from, const folly::fbstring &to)
{
    return folly::via(m_executor.get(),
        [ from = root(from), to = root(to), uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::link, from.c_str(), to.c_str());
        });
}

folly::Future<folly::Unit> PosixHelper::chmod(
    const folly::fbstring &fileId, const mode_t mode)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), mode, uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::chmod, filePath.c_str(), mode);
        });
}

folly::Future<folly::Unit> PosixHelper::chown(
    const folly::fbstring &fileId, const uid_t uid, const gid_t gid)
{
    return folly::via(m_executor.get(), [
        filePath = root(fileId), argUid = uid, argGid = gid, uid = m_uid,
        gid = m_gid
    ] {
        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException(EDOM);

        return setResult(::chown, filePath.c_str(), argUid, argGid);
    });
}

folly::Future<folly::Unit> PosixHelper::truncate(
    const folly::fbstring &fileId, const off_t size)
{
    return folly::via(m_executor.get(),
        [ filePath = root(fileId), size, uid = m_uid, gid = m_gid ] {
            UserCtxSetter userCTX{uid, gid};
            if (!userCTX.valid())
                return makeFuturePosixException(EDOM);

            return setResult(::truncate, filePath.c_str(), size);
        });
}

folly::Future<FileHandlePtr> PosixHelper::open(
    const folly::fbstring &fileId, const int flags, const Params &)
{
    return folly::via(m_executor.get(), [
        fileId, filePath = root(fileId), flags, executor = m_executor,
        uid = m_uid, gid = m_gid, timeout = m_timeout
    ]() mutable {
        UserCtxSetter userCTX{uid, gid};
        if (!userCTX.valid())
            return makeFuturePosixException<FileHandlePtr>(EDOM);

        int res = ::open(filePath.c_str(), flags);

        if (res == -1)
            return makeFuturePosixException<FileHandlePtr>(errno);

        auto handle = std::make_shared<PosixFileHandle>(std::move(fileId), uid,
            gid, res, std::move(executor), std::move(timeout));

        return folly::makeFuture<FileHandlePtr>(std::move(handle));
    });
}

} // namespace helpers
} // namespace one