/**
 * @file glusterfsHelperProxy.cc
 * @author Bartek Kryza
 * @copyright (C) 2017 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "glusterfsHelper.h"

#include <asio/buffer.hpp>
#include <asio/io_service.hpp>
#include <asio/ts/executor.hpp>
#include <boost/make_shared.hpp>
#include <boost/python.hpp>
#include <boost/python/extract.hpp>
#include <boost/python/raw_function.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include <iostream>

using ReadDirResult = std::vector<std::string>;

using namespace boost::python;
using namespace one::helpers;

/*
 * Minimum 2 threads are required to run helper.
 */
constexpr int GLUSTERFS_HELPER_WORKER_THREADS = 8;

class ReleaseGIL {
public:
    ReleaseGIL()
        : threadState{PyEval_SaveThread(), PyEval_RestoreThread}
    {
    }

private:
    std::unique_ptr<PyThreadState, decltype(&PyEval_RestoreThread)> threadState;
};

class GlusterFSHelperProxy {
public:
    GlusterFSHelperProxy(std::string mountPoint, uid_t uid, gid_t gid,
        std::string hostname, int port, std::string volume,
        std::string transport, std::string xlatorOptions)
        : m_service{GLUSTERFS_HELPER_WORKER_THREADS}
        , m_idleWork{asio::make_work_guard(m_service)}
        , m_helper{std::make_shared<one::helpers::GlusterFSHelper>(mountPoint,
              uid, gid, hostname, port, volume, transport, xlatorOptions,
              std::make_shared<one::AsioExecutor>(m_service))}
    {
        for (int i = 0; i < GLUSTERFS_HELPER_WORKER_THREADS; i++) {
            m_workers.push_back(std::thread([=]() { m_service.run(); }));
        }
    }

    ~GlusterFSHelperProxy()
    {
        m_service.stop();
        for (auto &worker : m_workers) {
            worker.join();
        }
    }

    void open(std::string fileId, int flags)
    {
        ReleaseGIL guard;
        m_helper->open(fileId, flags, {})
            .then(
                [&](one::helpers::FileHandlePtr handle) { handle->release(); });
    }

    std::string read(std::string fileId, int offset, int size)
    {
        ReleaseGIL guard;
        return m_helper->open(fileId, O_RDONLY, {})
            .then([&](one::helpers::FileHandlePtr handle) {
                auto buf = handle->read(offset, size).get();
                std::string data;
                buf.appendToString(data);
                return data;
            })
            .get();
    }

    int write(std::string fileId, std::string data, int offset)
    {
        ReleaseGIL guard;
        return m_helper->open(fileId, O_WRONLY | O_CREAT, {})
            .then([&](one::helpers::FileHandlePtr handle) {
                folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
                buf.append(data);
                return handle->write(offset, std::move(buf)).get();
            })
            .get();
    }

    struct stat getattr(std::string fileId)
    {
        ReleaseGIL guard;
        return m_helper->getattr(fileId).get();
    }

    void access(std::string fileId, int mask)
    {
        ReleaseGIL guard;
        m_helper->access(fileId, mask).get();
    }

    ReadDirResult readdir(std::string fileId, int offset, int count)
    {
        ReleaseGIL guard;
        std::vector<std::string> res;
        for (auto &direntry : m_helper->readdir(fileId, offset, count).get()) {
            res.emplace_back(direntry.toStdString());
        }
        return res;
    }

    std::string readlink(std::string fileId)
    {
        ReleaseGIL guard;
        return m_helper->readlink(fileId).get().toStdString();
    }

    void mknod(std::string fileId, mode_t mode, std::vector<Flag> flags)
    {
        ReleaseGIL guard;
        m_helper->mknod(fileId, mode, FlagsSet(flags.begin(), flags.end()), 0)
            .get();
    }

    void mkdir(std::string fileId, mode_t mode)
    {
        ReleaseGIL guard;
        m_helper->mkdir(fileId, mode).get();
    }

    void unlink(std::string fileId, int size)
    {
        ReleaseGIL guard;
        m_helper->unlink(fileId, size).get();
    }

    void rmdir(std::string fileId)
    {
        ReleaseGIL guard;
        m_helper->rmdir(fileId).get();
    }

    void symlink(std::string from, std::string to)
    {
        ReleaseGIL guard;
        m_helper->symlink(from, to).get();
    }

    void rename(std::string from, std::string to)
    {
        ReleaseGIL guard;
        m_helper->rename(from, to).get();
    }

    void link(std::string from, std::string to)
    {
        ReleaseGIL guard;
        m_helper->link(from, to).get();
    }

    void chmod(std::string fileId, mode_t mode)
    {
        ReleaseGIL guard;
        m_helper->chmod(fileId, mode).get();
    }

    void chown(std::string fileId, uid_t uid, gid_t gid)
    {
        ReleaseGIL guard;
        m_helper->chown(fileId, uid, gid).get();
    }

    void truncate(std::string fileId, int offset, int size)
    {
        ReleaseGIL guard;
        m_helper->truncate(fileId, offset, size).get();
    }

    std::string getxattr(std::string fileId, std::string name)
    {
        ReleaseGIL guard;
        return m_helper->getxattr(fileId, name).get().toStdString();
    }

    void setxattr(std::string fileId, std::string name, std::string value,
        bool create, bool replace)
    {
        ReleaseGIL guard;
        m_helper->setxattr(fileId, name, value, create, replace).get();
    }

    void removexattr(std::string fileId, std::string name)
    {
        ReleaseGIL guard;
        m_helper->removexattr(fileId, name).get();
    }

    std::vector<std::string> listxattr(std::string fileId)
    {
        ReleaseGIL guard;
        std::vector<std::string> res;
        for (auto &xattr : m_helper->listxattr(fileId).get()) {
            res.emplace_back(xattr.toStdString());
        }
        return res;
    }

private:
    asio::io_service m_service;
    asio::executor_work_guard<asio::io_service::executor_type> m_idleWork;
    std::vector<std::thread> m_workers;
    std::shared_ptr<one::helpers::GlusterFSHelper> m_helper;
};

namespace {
boost::shared_ptr<GlusterFSHelperProxy> create(std::string mountPoint,
    uid_t uid, gid_t gid, std::string hostname, int port, std::string volume,
    std::string transport, std::string xlatorOptions)
{
    return boost::make_shared<GlusterFSHelperProxy>(std::move(mountPoint), uid,
        gid, std::move(hostname), std::move(port), std::move(volume),
        std::move(transport), std::move(xlatorOptions));
}
} // namespace

BOOST_PYTHON_MODULE(glusterfs_helper)
{
    class_<GlusterFSHelperProxy, boost::noncopyable>(
        "GlusterFSHelperProxy", no_init)
        .def("__init__", make_constructor(create))
        .def("open", &GlusterFSHelperProxy::open)
        .def("read", &GlusterFSHelperProxy::read)
        .def("write", &GlusterFSHelperProxy::write)
        .def("getattr", &GlusterFSHelperProxy::getattr)
        .def("readdir", &GlusterFSHelperProxy::readdir)
        .def("readlink", &GlusterFSHelperProxy::readlink)
        .def("mknod", &GlusterFSHelperProxy::mknod)
        .def("mkdir", &GlusterFSHelperProxy::mkdir)
        .def("unlink", &GlusterFSHelperProxy::unlink)
        .def("rmdir", &GlusterFSHelperProxy::rmdir)
        .def("symlink", &GlusterFSHelperProxy::symlink)
        .def("rename", &GlusterFSHelperProxy::rename)
        .def("link", &GlusterFSHelperProxy::link)
        .def("chmod", &GlusterFSHelperProxy::chmod)
        .def("chown", &GlusterFSHelperProxy::chown)
        .def("truncate", &GlusterFSHelperProxy::truncate)
        .def("getxattr", &GlusterFSHelperProxy::getxattr)
        .def("setxattr", &GlusterFSHelperProxy::setxattr)
        .def("removexattr", &GlusterFSHelperProxy::removexattr)
        .def("listxattr", &GlusterFSHelperProxy::listxattr);
}
