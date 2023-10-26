#include <fcntl.h>
#include <liburing.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <coroutine>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "ThreadPool.h"
#include "tiny_obj_loader.h"

class ReadOnlyFile {
public:
    ReadOnlyFile(const std::string& file_path) : m_path(file_path) {
        m_fd = open(file_path.c_str(), O_RDONLY);
        if (m_fd < 0) {
            throw std::runtime_error("file open failed.");
        }
        m_size = get_file_size(m_fd);
        if (m_size < 0) {
            throw std::runtime_error("file size error.");
        }
    }
    ~ReadOnlyFile() {
        if (m_fd) {
            close(m_fd);
        }
    }
    int fd() const {
        return m_fd;
    }
    std::string path() const {
        return m_path;
    }
    off_t size() const {
        return m_size;
    }

private:
    int         m_fd;
    std::string m_path;
    off_t       m_size;

    off_t get_file_size(int fd) {
        struct stat s;
        if (fstat(fd, &s) != -1) {
            return s.st_size;
        }
        return -1;
    }
};  // class ReadOnlyFile

struct Result {
    int                statue_code{0};  //返回码
    tinyobj::ObjReader result;          //解析结果
    std::string        file;            //文件
};

//用reader解析buf
void readObjFromBuffer(const std::vector<char>& buf,
                       tinyobj::ObjReader&      reader) {
    auto s = std::string(buf.data(), buf.size());
    reader.ParseFromString(s, std::string{});
}

//----------第一种解析方法:简单阻塞解析--------------
Result readSyschronous(const ReadOnlyFile& file) {
    Result            result{.file = file.path()};
    std::vector<char> buf(file.size());
    read(file.fd(), buf.data(), buf.size());  // block
    readObjFromBuffer(buf, result.result);
    return result;
}

std::vector<Result>
trivialApproach(const std::vector<ReadOnlyFile>& files) {
    std::vector<Result> results;
    for (auto& file : files) {
        results.push_back(readSyschronous(file));
    }
    return results;
}

//-------第二种解析方法:利用io_uring批量提交打开文件任务，解析----------

class IOUring {
private:
    struct io_uring m_ring;

public:
    explicit IOUring(size_t queue_size) {
        int q = io_uring_queue_init(queue_size, &m_ring, 0);
        if (q < 0) {
            std::runtime_error("create io_uring failed");
        }
    }
    ~IOUring() {
        io_uring_queue_exit(&m_ring);
    }

    IOUring(const IOUring&) = delete;
    IOUring& operator=(const IOUring&) = delete;
    IOUring(IOUring&&) = delete;
    IOUring& operator=(IOUring&&) = delete;

    struct io_uring* get_ring() {
        return &m_ring;
    }
};

std::vector<std::vector<char>>
initialBuffer(const std::vector<ReadOnlyFile>& files) {
    std::vector<std::vector<char>> bufs;
    bufs.reserve(files.size());
    for (const auto& file : files) {
        bufs.emplace_back(file.size());
    }
    return bufs;
}

struct IdRequest {
    size_t id;
};

void pushEntriesToSubmissionQueue(std::vector<ReadOnlyFile>&      files,
                                  std::vector<std::vector<char>>& bufs,
                                  IOUring&                        ring) {
    for (size_t i = 0; i < files.size(); ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(ring.get_ring());
        io_uring_prep_read(sqe, files[i].fd(), bufs[i].data(),
                           bufs[i].size(), 0);
        IdRequest* request = new IdRequest{i};
        io_uring_sqe_set_data(sqe, request);
    }
}

std::vector<Result>
readEntriesFromCompletionQueue(std::vector<ReadOnlyFile>&      files,
                               std::vector<std::vector<char>>& bufs,
                               IOUring&                        ring) {
    std::vector<Result> results;
    results.reserve(files.size());
    while (results.size() < files.size()) {
        io_uring_submit_and_wait(ring.get_ring(), 1);
        io_uring_cqe* cqe;
        unsigned int  head;  // unused
        int           processed{0};
        io_uring_for_each_cqe(ring.get_ring(), head, cqe) {
            IdRequest* req = (IdRequest*)io_uring_cqe_get_data(cqe);
            results.push_back(
                {.statue_code = cqe->res, .file = files[req->id].path()});
            if (results.back().statue_code) {
                readObjFromBuffer(bufs[req->id], results.back().result);
            }
            processed++;
        }
        io_uring_cq_advance(ring.get_ring(), processed);
    }
    return results;
}

std::vector<Result> iouringObjLoader(std::vector<ReadOnlyFile>& files) {
    IOUring ring{files.size()};
    auto    bufs = initialBuffer(files);
    //把文件读取请求提交到请求队列(准备好，未提交)
    pushEntriesToSubmissionQueue(files, bufs, ring);
    //等待请求到达完成队列之后解析
    return readEntriesFromCompletionQueue(files, bufs, ring);
}

//--------------------------协程-----------------------------------

struct Request {
    std::coroutine_handle<> handle;
    int                     statusCode{-1};
};

class ReadFileAwaitable {
private:
    io_uring_sqe* m_sqe;

public:
    ReadFileAwaitable(IOUring& ring, const ReadOnlyFile& file,
                      std::vector<char>& buf) {
        m_sqe = io_uring_get_sqe(ring.get_ring());
        io_uring_prep_read(m_sqe, file.fd(), buf.data(), buf.size(), 0);
    }

    auto operator co_await() {
        struct Awaiter {
            io_uring_sqe* entry;  //访问m_sqe 附加handle
            Request       req;

            Awaiter(io_uring_sqe* sqe) : entry(sqe) {}

            bool await_ready() {  //总是暂停
                return false;
            }
            void await_suspend(std::coroutine_handle<> handle) {
                req.handle = handle;
                io_uring_sqe_set_data(entry, &req);
            }
            int await_resume() {
                return req.statusCode;
            }
        };

        return Awaiter{m_sqe};
    }
};

int consumeCQENonBlocking(IOUring& ring) {
    io_uring_cqe* tmp;
    if (io_uring_peek_cqe(ring.get_ring(), &tmp) != 0) {
        return 0;
    }
    int           processed{0};
    io_uring_cqe* cqe;
    unsigned      head;  // unuse
    io_uring_for_each_cqe(ring.get_ring(), head, cqe) {
        Request* req = static_cast<Request*>(io_uring_cqe_get_data(cqe));
        req->statusCode = cqe->res;
        req->handle.resume();
        processed++;
    }
    io_uring_cq_advance(ring.get_ring(), processed);
    return processed;
}

class Task {
public:
    struct promise_type {
        Result m_result;  //传递结果

        void return_value(Result& result) {
            m_result = std::move(result);
        }
        Task get_return_object() {
            return Task(this);
        }

        std::suspend_never initial_suspend() {
            return {};
        }
        std::suspend_always final_suspend() noexcept {
            return {};
        }
        void unhandled_exception() {}
    };

    //生成协程的handle
    explicit Task(promise_type* promise)
        : m_handle(
              std::coroutine_handle<promise_type>::from_promise(*promise)) {
    }
    Task(Task&& other) : m_handle(std::exchange(other.m_handle, nullptr)) {}

    ~Task() {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    Result getReuslt() const {
        assert(m_handle.done());
        return m_handle.promise().m_result;
    }

    bool done() const {
        return m_handle.done();
    }

    std::coroutine_handle<promise_type> m_handle;
};

Task parseOBJFile(IOUring& ring, const ReadOnlyFile& file,
                  ThreadPool& pool) {
    std::vector<char> buf(file.size());
    int               status = co_await ReadFileAwaitable{ring, file, buf};
    //----这里可以在线程池中运行，并行解析----
    co_await pool.schedule();
    Result result{.statue_code = 0, .file = file.path()};
    readObjFromBuffer(buf, result.result);
    co_return result;
}

bool allDone(const std::vector<Task>& tasks) {
    return std::all_of(tasks.cbegin(), tasks.cend(),
                       [](const auto& t) { return t.done(); });
}

std::vector<Result> parseOBJFiles(std::vector<ReadOnlyFile> files) {
    IOUring           ring(files.size());
    ThreadPool        pool;
    std::vector<Task> tasks;
    for (const auto& file : files) {
        tasks.push_back(parseOBJFile(ring, file, pool));
    }
    io_uring_submit(ring.get_ring());
    while (!allDone(tasks)) {
        consumeCQENonBlocking(ring);
    }
    std::vector<Result> results;
    results.reserve(files.size());
    for (auto&& t : tasks) {
        results.push_back(std::move(t).getReuslt());
    }
    return results;
}

int main(int argc, char* argv[]) {
    std::vector<ReadOnlyFile> files;
    if (argc == 1) {
        std::cout << "the arg is less!!!!" << std::endl;
        return 0;
    }
    for (int i = 0; i < 10; i++) {
        files.emplace_back("/home/yjc/YJC_PROJECTS/objLoader/cactus.obj");
    }
    if (std::string(argv[1]) == "1") {
        auto results = trivialApproach(files);
    } else if (std::string(argv[1]) == "2") {
        auto results = iouringObjLoader(files);
    } else if (std::string(argv[1]) == "3") {
        auto results = parseOBJFiles(files);
    }
}
