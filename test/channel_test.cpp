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

DEFINE_string(taskid, "channel_test", "task id, defalut is 'channel_test'");
DEFINE_int32(role, -1, "role, defalut value is -1, mean run all role");
DEFINE_string(type, "grpc", "channel type: 'grpc', 'mem', current default is 'grpc'");

DEFINE_int32(enable_cv, 1, "use condition variables instead spinlock");
DEFINE_string(redis_uri, "tcp://redis123@10.100.66.68:9379", "redis url, default is 'tcp://127.0.0.1:6379'");
DEFINE_string(server_addr, "10.100.66.68:6800", "grpc server addr, default is '127.0.0.1:9900'");
DEFINE_int32(log_level, 3, "log level, 0-6, default is 0");

static inline std::unique_ptr<gaianet::IChannel> create_channel(const std::string& taskid, uint32_t from, uint32_t to) {
    if (FLAGS_type == "grpc") {
        return std::unique_ptr<gaianet::IChannel>(new gaianet::channel(from, to, taskid, FLAGS_server_addr, FLAGS_redis_uri));
    } else if (FLAGS_type == "mem") {
        return std::unique_ptr<gaianet::IChannel>(new gaianet::MemChannel(from, to, taskid, FLAGS_enable_cv != 0));
    } else {
        assert(false);
    }
}

static inline std::string get_rand_string(uint32_t* seed, int len) {
    std::string ret;
    ret.reserve(len);
    for (int i = 0; i < len; ++i) {
        ret.push_back(rand_r(seed));
    }
    return ret;
}

static void test_send(const std::string& taskid, uint32_t data_seed) {
    try {
        auto chl = create_channel(taskid, 0, 1);
        for (int i = 0; i < 5; ++i) {
            size_t size = 2ul << (24 + i);
            std::string send_buf = get_rand_string(&data_seed, size);
            chl->send(send_buf);
        }
    } catch (...) {
        // gaialog_error("send get a exception...");
    }
}

static void test_recv(const std::string& taskid, uint32_t verify_seed) {
    try {
        auto chl = create_channel(taskid, 1, 0);
        for (int i = 0; i < 5; ++i) {
            size_t size = 2ul << (24 + i);

            std::string verify_str = get_rand_string(&verify_seed, size);

            std::string recv_buf;
            chl->recv(recv_buf);

            if (verify_str != recv_buf) {
                // gaialog_error("recv data error");
            }
        }
    } catch (...) {
        // gaialog_error("recv get a exception...");
    }
}

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    // gaialog::init("channel_test", ".", true, FLAGS_log_level, 0, false);

    const uint32_t seed = 0;
    const std::string taskid(FLAGS_taskid);

    if (FLAGS_role == -1) {
        std::thread t1(test_send, taskid, seed);
        std::thread t2(test_recv, taskid, seed);
        t1.join();
        t2.join();
    } else if (FLAGS_role == 0) {
        test_send(taskid, seed);
    } else if (FLAGS_role == 1) {
        test_recv(taskid, seed);
    } else {
        assert(false);
    }

    return 0;
}
