#include "channel.h"
#include "memchannel.h"
#include <cassert>
#include <chrono>
// #include <// gaialog.h>
#include <gflags/gflags.h>
#include <memory>
#include <shared_mutex>
#include <signal.h>
#include <thread>
#include <fmt/format.h>
DEFINE_string(taskid, "channel_bench_cpp_1", "task id, defalut is 'channel_bench_cpp_'");
DEFINE_int32(role, -1, "role, defalut value is -1, mean run all role");
DEFINE_string(type, "mem", "channel type: 'grpc', 'mem', current default is 'grpc'");
DEFINE_int32(max_packet_size, 1024 * 16, "max packet size");
DEFINE_int32(loop_cnt, 0, "loop count, default 0 mean no limit");
DEFINE_int32(enable_cv, 1, "use condition variables instead spinlock");
DEFINE_int32(thread, 1, "test thread count");
DEFINE_string(redis_uri, "tcp://redis123@10.100.66.68:9379", "redis url, default is 'tcp://redis123@10.100.66.68:9379'");
DEFINE_string(server_addr, "10.100.66.68:6800", "grpc server addr, default is '10.100.66.68:6800'");

//
// grpc channel 20MB/s
// grpc channel + async server 100MB/s
// mem channel 200MB/s

volatile sig_atomic_t gSignalStatus;

void signal_handler(int signal) {
    gSignalStatus = signal;
}

static inline std::string get_rand_string(uint32_t* seed) {
    // int len = 4096 * 16 - 4;
    int len = rand_r(seed) % FLAGS_max_packet_size;

    std::string ret;
    ret.reserve(len);
    for (int i = 0; i < len; ++i) {
        ret.push_back(rand_r(seed));
    }
    return ret;
}

static inline std::string get_rand_string_fast(uint32_t* seed) {
    static const std::string init_string = [seed]() {
        uint32_t init_seed = *seed;
        std::string ret;
        ret.reserve(FLAGS_max_packet_size);
        for (int i = 0; i < FLAGS_max_packet_size; ++i) {
            ret.push_back(rand_r(&init_seed));
        }
        return ret;
    }();

    int len = rand_r(seed) % FLAGS_max_packet_size;
    return init_string.substr(0, len);
}

static inline std::unique_ptr<gaianet::IChannel> create_channel(const std::string& taskid, uint32_t from, uint32_t to) {
    // auto FLAGS_type  = "mem";
    if (FLAGS_type == "grpc") {
        return std::unique_ptr<gaianet::IChannel>(new gaianet::channel(from, to, taskid, FLAGS_server_addr, FLAGS_redis_uri));
    } else if (FLAGS_type == "mem") {
        return std::unique_ptr<gaianet::IChannel>(new gaianet::MemChannel(from, to, taskid, FLAGS_enable_cv != 0));
    } else {
        assert(false);
    }
}

static std::string size_to_str(std::size_t size) {
    double f_size = size;
    if (f_size < 1000) {
        return fmt::format("{0:.2f} B", f_size);
    } else if (f_size < 1000 * 1000) {
        return fmt::format("{0:.2f} KB", f_size / 1000);
    } else if (f_size < 1000 * 1000 * 1000) {
        return fmt::format("{0:.2f} MB", f_size / 1000 / 1000);
    } else {
        return fmt::format("{0:.2f} GB", f_size / 1000 / 1000 / 1000);
    }
}

struct Stat {
    std::size_t send_total_size;
    std::size_t send_cur_size;
    std::size_t recv_total_size;
    std::size_t recv_cur_size;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::time_point t1;
    std::shared_mutex t1_rwlock;
};

static Stat stats[2];

static void test_role(const std::string& taskid, uint32_t data_seed, int role) {
    auto chl = create_channel(taskid, role, 1 - role);

    stats[role].t0 = std::chrono::steady_clock::now();
    stats[role].t1 = stats[role].t0;

    if (role == 0) {
        chl->send("hello", 5);
        std::string str;
        chl->recv(str);
        assert(str == "hello");
    } else {
        std::string str;
        chl->recv(str);
        chl->send(str);
    }

    for (int i = 0; FLAGS_loop_cnt == 0 || i < FLAGS_loop_cnt; ++i) {
        std::string rand_str = get_rand_string_fast(&data_seed);
        bool is_send = static_cast<bool>((data_seed + role) % 2);

        if (is_send) {
            auto bytes_send = rand_str.length();
            chl->send(rand_str);
            stats[role].send_cur_size += bytes_send;
        } else {
            std::string recv_str;
            chl->recv(recv_str);
            assert(recv_str == rand_str);
            stats[role].recv_cur_size += rand_str.length();
        }

        auto t2 = std::chrono::steady_clock::now();
        stats[role].t1_rwlock.lock_shared();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - stats[role].t1);
        stats[role].t1_rwlock.unlock_shared();

        if (ms.count() > 1000) {
            stats[role].t1_rwlock.lock();
            stats[role].t1 = t2;
            stats[role].send_total_size += stats[role].send_cur_size;
            stats[role].recv_total_size += stats[role].recv_cur_size;
            const std::size_t cur_send_speed = stats[role].send_cur_size * 1000 / ms.count();
            const std::size_t avg_send_speed =
                stats[role].send_total_size * 1000 / std::chrono::duration_cast<std::chrono::milliseconds>(t2 - stats[role].t0).count();
            const std::size_t cur_recv_speed = stats[role].recv_cur_size * 1000 / ms.count();
            const std::size_t avg_recv_speed =
                stats[role].recv_total_size * 1000 / std::chrono::duration_cast<std::chrono::milliseconds>(t2 - stats[role].t0).count();
            stats[role].send_cur_size = 0;
            stats[role].recv_cur_size = 0;
            stats[role].t1_rwlock.unlock();
            printf("role:%d send avg: %s/s  cur: %s/s  recv avg: %s/s cur: %s/s  total: %s %s\n", role, size_to_str(avg_send_speed).c_str(),
                   size_to_str(cur_send_speed).c_str(), size_to_str(avg_recv_speed).c_str(), size_to_str(cur_recv_speed).c_str(),
                   size_to_str(stats[role].send_total_size).c_str(), size_to_str(stats[role].recv_total_size).c_str());
        }
    }
}

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    const uint32_t seed = 0;
    const std::string taskid(FLAGS_taskid);

    auto test_func = [](const std::string& taskid, uint32_t seed) {
        if (FLAGS_role == -1) {
            uint32_t t0_seed = seed;
            uint32_t t1_seed = seed;
            std::thread t1(test_role, taskid, t0_seed, 0);
            std::thread t2(test_role, taskid, t1_seed, 1);
            t1.join();
            t2.join();
        } else if (FLAGS_role == 0) {
            test_role(taskid, seed, 0);
        } else if (FLAGS_role == 1) {
            test_role(taskid, seed, 1);
        } else {
            assert(false);
        }
    };

    if (FLAGS_thread > 1) {
        std::vector<std::thread> all_threads;

        for (int i = 0; i < FLAGS_thread; ++i) {
            all_threads.emplace_back(test_func, taskid + std::to_string(i), seed);
        }

        for (auto& t : all_threads) {
            t.join();
        }
    } else {
        test_func(taskid, seed);
    }

    return 0;
}
