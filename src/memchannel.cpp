#include "memchannel.h"

// #include "// gaialog.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gaia_utils/fast_resize.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <cstring> 
#if defined __aarch64__
constexpr std::size_t platform_cache_line_size = 64;
#elif defined __x86_64__
constexpr std::size_t platform_cache_line_size = 128;
#else
#error "unknown platform"
#endif

#if defined __APPLE__

#elif defined __linux__
#else
#error "not support os"
#endif

namespace gaianet {

    namespace detail {
        struct SharedMemoryView {
            uint32_t buf_size;
            bool enable_cv;
            std::atomic<int> client_attached;
            pthread_mutex_t mutex;
            pthread_cond_t cv;

            alignas(platform_cache_line_size) std::atomic<uint32_t> in;
            alignas(platform_cache_line_size) std::atomic<uint32_t> out;

            const uint8_t buffer[0];

            void init(uint32_t arg_buf_size, bool arg_enable_cv) {
                buf_size = arg_buf_size;
                enable_cv = arg_enable_cv;
                client_attached = 0;

                if (arg_enable_cv) {

                    int r;

                    pthread_condattr_t cond_attr;
                    r = pthread_condattr_init(&cond_attr);
                    assert(r == 0);
                    r = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
                    assert(r == 0);
                    r = pthread_cond_init(&cv, &cond_attr);
                    assert(r == 0);
                    r = pthread_condattr_destroy(&cond_attr);
                    assert(r == 0);

                    pthread_mutexattr_t m_attr;
                    r = pthread_mutexattr_init(&m_attr);
                    assert(r == 0);
                    r = pthread_mutexattr_setpshared(&m_attr, PTHREAD_PROCESS_SHARED);
                    assert(r == 0);
                    r = pthread_mutex_init(&mutex, &m_attr);
                    assert(r == 0);
                    r = pthread_mutexattr_destroy(&m_attr);
                    assert(r == 0);
                }

                in = 0;
                out = 0;
            }

            void attach_init(bool arg_enable_cv) {
                assert(++client_attached == 1);
                assert(enable_cv == arg_enable_cv);
            }

            void put(const uint8_t* buf, uint32_t len) {
                uint32_t l;
                len = std::min(len, buf_size - in + out);
                l = std::min(len, buf_size - (in & (buf_size - 1)));
                memcpy((void*)(buffer + (in & (buf_size - 1))), buf, l);
                memcpy((void*)buffer, buf + l, len - l);
                in += len;
            }

            void get(uint8_t* buf, uint32_t len) {
                uint32_t l;
                len = std::min(len, in - out);
                l = std::min(len, buf_size - (out & (buf_size - 1)));
                memcpy(buf, buffer + (out & (buf_size - 1)), l);
                memcpy(buf + l, buffer, len - l);
                out += len;
            }

            bool can_put(uint32_t size) {
                return (in - out) + size <= buf_size;
            }

            bool can_get(uint32_t size) {
                return (in - out) >= size;
            }

            void read(uint8_t* buf, uint32_t len) {
                if (enable_cv) {
                    pthread_mutex_lock(&mutex);
                    while (!can_get(len)) {
                        pthread_cond_wait(&cv, &mutex);
                    }
                    get(buf, len);
                    pthread_mutex_unlock(&mutex);
                    pthread_cond_signal(&cv);
                } else {
                    while (!can_get(len))
                        ;
                    get(buf, len);
                }
            }

            void write(const uint8_t* buf, uint32_t len) {
                if (enable_cv) {
                    pthread_mutex_lock(&mutex);
                    while (!can_put(len)) {
                        pthread_cond_wait(&cv, &mutex);
                    }
                    put(buf, len);
                    pthread_mutex_unlock(&mutex);
                    pthread_cond_signal(&cv);

                } else {
                    while (!can_put(len))
                        ;
                    put(buf, len);
                }
            }
        };
    } // namespace detail

    static std::mutex s_key_lock_global_mtx;
    static std::map<std::string, std::pair<std::atomic<int>, std::mutex>> s_key_lock_global_map;

    static inline void get_key_lock(const std::string& key) {
        auto lg = std::unique_lock(s_key_lock_global_mtx);
        auto& pair = s_key_lock_global_map[key];
        pair.first++;
        lg.unlock();
        pair.second.lock();
    }

    static inline void free_key_lock(const std::string& key) {
        auto lg = std::unique_lock(s_key_lock_global_mtx);
        auto iter = s_key_lock_global_map.find(key);
        assert(iter != s_key_lock_global_map.end());

        auto& pair = iter->second;
        pair.second.unlock();

        if (--pair.first == 0) {
            s_key_lock_global_map.erase(key);
        }
        lg.unlock();
    }

    static inline int get_file_lock(const std::string& key) {
#if defined __APPLE__
        auto tmp_path = std::filesystem::temp_directory_path().string();
#elif defined __linux__
        auto tmp_path = std::filesystem::temp_directory_path().string() + "/";
#endif
        std::string lock_file = tmp_path + key + ".lck";
        int fd = open(lock_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            int ec = errno;
            // gaialog_assert_msg(false, "get_file_lock open failed, lock_file:{},errno:{},errmsg:{}", lock_file, ec, strerror(ec));
        } else {
            for (;;) {
                struct flock fd_lock;
                fd_lock.l_type = F_WRLCK;
                fd_lock.l_whence = SEEK_SET;
                fd_lock.l_start = 0;
                fd_lock.l_len = 0;
                fd_lock.l_pid = 0;
                int ret = fcntl(fd, F_SETLKW, &fd_lock); // not across fork/exec
                if (ret != -1) {
                    break;
                } else {
                    int ec = errno;
                    // gaialog_assert_msg(false, "get_file_lock fcntl failed, lock_file:{},errno:{},errmsg:{}", lock_file, ec, strerror(ec));
                }
            }
        }
        // when in same process
        get_key_lock(key);

        return fd;
    }

    static inline void free_file_lock(const std::string& key, int fd) {
        free_key_lock(key);
        int flag = close(fd);
        if (flag != 0) {
            int ec = errno;
            // gaialog_assert_msg(false, "free_file_lock close failed, errno:{},errmsg:{}", ec, strerror(ec));
        }
    }

    static inline void delete_file_lock(const std::string& key) {
        auto tmp_path = std::filesystem::temp_directory_path().string() + "/";
        std::string lock_file = tmp_path + key + ".lck";
        int flag = unlink(lock_file.c_str());
        if (flag != 0) {
            int ec = errno;
            // gaialog_assert_msg(false, "delete_file_lock, errno:{},errmsg:{}", ec, strerror(ec));
        }
    }

    static inline void destroy_shm(const std::string& key) {
        shm_unlink(key.c_str());
    }

    static inline std::pair<detail::SharedMemoryView*, int> create_shm(const std::string& key, size_t buf_size, bool enable_cv) {
        int lock_fd = get_file_lock(key);
        int fd = shm_open(key.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
        if (fd == -1) {
            int ec = errno;
            // gaialog_assert_msg(ec == EEXIST, "shm_open open failed, key:{}, errno:{},errmsg:{}", key, ec, strerror(ec));
            fd = shm_open(key.c_str(), O_RDWR, 0666);
            ec = errno;
            // gaialog_assert_msg(fd != -1, "shm_open reopen failed, key:{}, errno:{},errmsg:{}", key, ec, strerror(ec));
        }
        int map_size = buf_size + sizeof(detail::SharedMemoryView);
        int code = ftruncate(fd, map_size);
        if (code != 0) {
            int ec = errno;
            // gaialog_assert_msg(false, "ftruncate failed, errno:{},errmsg:{}", ec, strerror(ec));
        }
        detail::SharedMemoryView* view = (detail::SharedMemoryView*)mmap(nullptr, map_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
        // gaialog_assert(view != MAP_FAILED);
        view->init(buf_size, enable_cv);
        free_file_lock(key, lock_fd);
        // gaialog_debug("create_shm {} succeed", key);
        return {view, fd};
    }

    static inline std::pair<detail::SharedMemoryView*, int> attach_shm(const std::string& key, size_t verify_buf_size, bool enable_cv) {
        detail::SharedMemoryView* view = nullptr;
        int fd = -1;

        while (true) {
            fd = shm_open(key.c_str(), O_RDWR, 0666);
            if (fd != -1) {
                int lock_fd = get_file_lock(key);
                int remote_size = lseek(fd, (size_t)0, SEEK_END);
                if (remote_size == -1) {
#if defined __APPLE__
                    remote_size = macos_buf_size + sizeof(detail::SharedMemoryView);
#elif defined __linux__
                    int ec = errno;
                    // gaialog_assert_msg(ec == ENOENT, "attach_shm lseek failed, errno:{},errmsg:{}", ec, strerror(ec));
#else
#endif
                }

                view = (detail::SharedMemoryView*)mmap(nullptr, remote_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
                if (view == MAP_FAILED) {
                    int ec = errno;
                    // gaialog_assert_msg(false, "attach_shm mmap size = {} failed, errno:{},errmsg:{}", remote_size, ec, strerror(ec));
                }

                uint32_t buf_size = view->buf_size;
                // gaialog_assert_msg(verify_buf_size == buf_size, "verify buf_size failed {} != {}", verify_buf_size, buf_size);
                view->attach_init(enable_cv);
                int r = shm_unlink(key.c_str());
                if (r != 0) {
                    int ec = errno;
                    // gaialog_assert_msg(false, "error code = {} {}", ec, strerror(ec));
                }
                free_file_lock(key, lock_fd);
                delete_file_lock(key);
                // gaialog_debug("attach_shm {} succeed", key);
                break;
            } else {
                int ec = errno;
                // gaialog_assert_msg(ec == ENOENT, "shm_open failed,key:{}, errno:{},errmsg:{}", key, ec, strerror(ec));
                // gaialog_debug("attach_shm:{} waiting...", key);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        return {view, fd};
    }

    static inline std::string get_shm_key(uint32_t from, uint32_t to, const std::string& taskid) {
        return "MC" + taskid + "_" + std::to_string(from) + "_" + std::to_string(to);
    }

    MemChannel::MemChannel(uint32_t from_party, uint32_t to_party, const std::string& taskid, bool enable_cv /* = true */, int buf_size /* = 1024 * 16 */)
        : IChannel(taskid, from_party, to_party)
        , m_buf_size(buf_size)
        , m_enable_cv(enable_cv) {
        if (get_from_party() < get_to_party()) {
            auto send_key = get_shm_key(get_from_party(), get_to_party(), get_channel_id());
            auto recv_key = get_shm_key(get_to_party(), get_from_party(), get_channel_id());
            destroy_shm(send_key);
            destroy_shm(recv_key);
            {
                auto pair = create_shm(send_key, m_buf_size, m_enable_cv);
                m_send_buf = pair.first;
                m_send_fd = pair.second;
            }
            {
                auto pair = create_shm(recv_key, m_buf_size, m_enable_cv);
                m_recv_buf = pair.first;
                m_recv_fd = pair.second;
            }
            need_attach = false;
        } else {
            need_attach = true;
            // do delay attach when call send/recv
            m_send_buf = nullptr;
            m_send_fd = -1;
            m_recv_buf = nullptr;
            m_recv_fd = -1;
        }

#if defined __APPLE__
        // gaialog_assert_msg(buf_size == macos_buf_size, "macos only support buf_size:{}", macos_buf_size);
#endif
        assert(buf_size != 0 && buf_size >= 8 && (buf_size & (buf_size - 1)) == 0); // must be 2^n
    }

    MemChannel::~MemChannel() {

        auto unmap = [this] {
            int r1 = munmap((void*)m_send_buf, m_send_buf->buf_size + sizeof(detail::SharedMemoryView));
            // gaialog_assert(r1 == 0);
            int r2 = munmap((void*)m_recv_buf, m_recv_buf->buf_size + sizeof(detail::SharedMemoryView));
            // gaialog_assert(r2 == 0);
        };

        if (get_from_party() < get_to_party()) {
            if (++m_send_buf->client_attached == 1) {
                auto send_key = get_shm_key(get_from_party(), get_to_party(), get_channel_id());
                shm_unlink(send_key.c_str());
            }

            if (++m_recv_buf->client_attached == 1) {
                auto recv_key = get_shm_key(get_to_party(), get_from_party(), get_channel_id());
                shm_unlink(recv_key.c_str());
            }
            unmap();
        } else {
            if (m_send_buf || m_recv_buf) {
                assert(m_send_buf && m_recv_buf);
                unmap();
            }
        }

        if (m_send_fd != -1) {
            close(m_send_fd);
        }

        if (m_recv_fd != -1) {
            close(m_recv_fd);
        }

        statistics_output();
    }

    void MemChannel::send(const void* buf, uint64_t nbytes) {
        if (need_attach) {
            attach();
        }
        auto view = m_send_buf;

        uint32_t body_len = nbytes;
        view->write((const uint8_t*)&body_len, sizeof(body_len));

        uint32_t chunk_size = m_buf_size;
        int chunk_cnt = (body_len + chunk_size - 1) / chunk_size;

        for (int i = 0, pos = 0; i < chunk_cnt; ++i) {
            uint32_t len = std::min(body_len, chunk_size);
            view->write((uint8_t*)buf + pos, len);
            pos += len;
            body_len -= len;
        }

        statistics_send(nbytes);
    }

    void MemChannel::send(std::string& str) {
        return send(str.data(), str.length());
    }

    void MemChannel::attach() {
        assert(need_attach);
        need_attach = false;
        // attach send
        {
            auto send_key = get_shm_key(get_from_party(), get_to_party(), get_channel_id());
            auto pair = attach_shm(send_key, m_buf_size, m_enable_cv);
            assert(pair.first != nullptr);
            assert(pair.second != -1);
            m_send_buf = pair.first;
            m_send_fd = pair.second;
        }
        // attach recv
        {
            auto recv_key = get_shm_key(get_to_party(), get_from_party(), get_channel_id());
            auto pair = attach_shm(recv_key, m_buf_size, m_enable_cv);
            assert(pair.first != nullptr);
            assert(pair.second != -1);
            m_recv_buf = pair.first;
            m_recv_fd = pair.second;
        }
    }

    void MemChannel::recv_body(void* buf, uint64_t length) {
        auto view = m_recv_buf;

        uint32_t body_len = length;
        uint32_t chunk_size = m_buf_size;
        int chunk_cnt = (body_len + chunk_size - 1) / chunk_size;

        for (int i = 0, pos = 0; i < chunk_cnt; ++i) {
            uint32_t len = std::min(body_len, chunk_size);
            view->read((uint8_t*)buf + pos, len);
            pos += len;
            body_len -= len;
        }

        statistics_recv(length);
    }

    void MemChannel::recv(std::string& str) {
        if (need_attach) {
            attach();
        }
        auto view = m_recv_buf;
        uint32_t body_len;
        view->read((uint8_t*)&body_len, sizeof(body_len));
        gaia_utils::fast_resize(str, body_len);
        recv_body(str.data(), str.length());
    }

    void MemChannel::recv(void* buf, uint64_t length) {
        if (need_attach) {
            attach();
        }
        auto view = m_recv_buf;
        uint32_t body_len;
        view->read((uint8_t*)&body_len, sizeof(body_len));
        assert(body_len == length);
        recv_body(buf, length);
    }

    bool MemChannel::flush() {
        return true;
    }

} // namespace gaianet
