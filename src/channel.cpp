
#include "channel.h"

// #include "typedefs.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <lz4.h>

#include "grpc_client.h"

// #include "// gaialog.h"
uint32_t CHUNK_SIZE = 500 * 1024 * 1024; // redis最大容纳512MB

static std::string get_lz4_compress_str(const char* data, int len) {
    std::string ret;
    int cap = LZ4_compressBound(len);
    ret.resize(cap + 4);

    int real_len = LZ4_compress_default(data, ret.data() + 4, len, cap);
    // gaialog_assert(real_len > 0);
    *(int*)ret.data() = len; // 发送原始长度，方便解压
    ret.resize(real_len + 4);

    // gaialog_trace("send compress {} ---> {}  {}", len, real_len, (double)real_len * 100 / len);
    return ret;
}

static std::string get_lz4_decompress_str(const char* data, int len) {
    std::string ret;
    int origin_len = *(int*)data;
    ret.resize(origin_len);

    int real_len = LZ4_decompress_safe(data + 4, ret.data(), len - 4, origin_len);
    // gaialog_assert(real_len == origin_len);

    // gaialog_trace("recv decompress {} ---> {}", len - 4, real_len);
    return ret;
}

namespace gaianet {
    channel::channel(uint32_t from_party, uint32_t to_party, const string& taskid, const string& server_address, const string& redis_address,
                     uint32_t connect_wait_time, bool use_redis, bool net_log_switch, const std::map<std::string, std::string>& meta)
        : IChannel(taskid, from_party, to_party)
        , m_server_address(server_address)
        , m_redis_address(redis_address)
        , m_connect_wait_time(connect_wait_time)
        , m_use_redis(use_redis)
        , m_net_log_switch(net_log_switch)
        , m_meta(meta) {
        // // gaialog::init(taskid.c_str(), ".");
        create_channel();
    }

    void channel::create_channel() {
        grpc::ChannelArguments channel_args;
        channel_args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, -1);
        channel_args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, -1);
        // 如果连接断开，重试策略为固定每秒1次
        channel_args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 1000);
        channel_args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 1000);
        channel_args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);

        // keep alive setting
        // https://grpc.github.io/grpc/cpp/md_doc_keepalive.html
        // https://github.com/grpc/proposal/blob/master/A8-client-side-keepalive.md

        // 不能大于server端的GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,否则会被服务端踢掉连接
        // 目前mesh配置为5s， grpc server配置为30s， 所以当前版本 GRPC_ARG_KEEPALIVE_TIME_MS必须大于30s
        // 后续先减小grpc server到5秒，全部节点升级后再改小这里的值到5-10秒，现在先设为35s
        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 35 * 1000);
        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10 * 1000);
        channel_args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);

        auto test_compress = getenv("TEST_GRPC_COMPRESS");
        if (test_compress) {
            channel_args.SetCompressionAlgorithm(grpc_compression_algorithm::GRPC_COMPRESS_GZIP);
        }

        //        channel_args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
        //        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 2000);
        //        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 60000);
        //        channel_args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

        auto chl = grpc::CreateCustomChannel(m_server_address, grpc::InsecureChannelCredentials(), channel_args);

        // gaialog_info("connect remote server {} wait time:{}", m_server_address, m_connect_wait_time);
        if (!chl->WaitForConnected(std::chrono::system_clock::now() + std::chrono::milliseconds(m_connect_wait_time))) {

            // gaialog_warn("connect remote server {} failed", m_server_address);
            throw "connect server " + m_server_address + " failed";
        } else {
            // gaialog_info("connect remote server {} succeed", m_server_address);
        }
        m_client = new gaianet::RpcClient(*this, chl, get_from_party(), get_to_party(), m_redis_address, m_use_redis, m_net_log_switch);

        if (!m_use_redis) {
            std::shared_ptr<grpc::Channel> chl1 = grpc::CreateCustomChannel(m_redis_address, grpc::InsecureChannelCredentials(), channel_args);

            // gaialog_info("connect local server {} wait time:{}", m_redis_address, m_connect_wait_time);
            if (!chl1->WaitForConnected(std::chrono::system_clock::now() + std::chrono::milliseconds(m_connect_wait_time))) {

                // gaialog_warn("connect local server {} failed", m_redis_address);
            } else {
                // gaialog_info("connect local server {} succeed", m_redis_address);
            }
            m_rcv_client = new gaianet::RpcClient(*this, chl1, get_from_party(), get_to_party(), m_redis_address, m_use_redis, m_net_log_switch);
        }
    }

    void channel::destroy_channel() {
        if (!m_use_redis) {
            m_rcv_client->finish();
            delete m_rcv_client;
            // gaialog_info("disconnect local server {} succeed", m_redis_address);
        }

        if (m_client) {
            m_client->finish();
            delete m_client;
            m_client = nullptr;
            // gaialog_info("disconnect remote server {} succeed", m_server_address);
        }
    }

    void channel::real_send(std::string& move_out_str) {
        uint64_t nbytes = move_out_str.length();
        system_clock::time_point start_time = system_clock::now();

        bool ret;
        for (int i = 0; i < 120; ++i) {
            ret = m_client->Send(get_channel_id(), move_out_str);
            if (ret) {
                break;
            } else {
                // gaialog_warn("send packet failed ! retrying ......................:{}", i);
                // ugly code...
                m_client->finish();
                auto continue_status = m_client->get_continue_status();
                delete m_client;
                m_client = nullptr;
                // gaialog_info("disconnect remote server {} succeed", m_server_address);
                destroy_channel();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                // gaialog_warn("reconnet to server with not ack data:{}", std::get<0>(continue_status).size());
                create_channel();
                ret = m_client->set_continue_status(continue_status);
                // gaialog_warn("send packet to server again with set_continue_status:{}", ret);
            }
        }

        if (ret) {
            system_clock::time_point end_time = system_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            if (secs.count() > 1000) {
                // gaialog_info("exec sender time used:{}", secs.count());
            }

            statistics_send(nbytes);
        } else {
            // gaialog_error("send packet failed!");
            throw "send data failed...";
        }
    }

    void channel::real_recv(std::string& move_in_str) {
        if (!m_use_redis) {
            while (1) {
                if (m_rcv_client->Recv(get_channel_id(), move_in_str)) {
                    break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    // gaialog_debug("not found queue sleep 20 ms");
                }
            }
        } else {
            system_clock::time_point start_time = system_clock::now();

            bool ret;
            for (int i = 0; i < 120; ++i) {
                bool need_retry = false;
                ret = m_client->Recv(get_channel_id(), move_in_str, &need_retry);
                if (ret) {
                    break;
                } else {
                    if (!need_retry)
                        break;
                    // gaialog_warn("recv packet failed ! retrying ......................:{}", i);
                    // ugly code...
                    m_client->finish();
                    auto continue_status = m_client->get_continue_status();
                    delete m_client;
                    m_client = nullptr;
                    // gaialog_info("disconnect remote server {} succeed", m_server_address);
                    destroy_channel();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    // gaialog_warn("reconnet to server with not ack data:{}", std::get<0>(continue_status).size());
                    create_channel();
                    ret = m_client->set_continue_status(continue_status);
                    // gaialog_warn("recv packet to server again with set_continue_status:{}", ret);
                }
            }

            if (!ret) {
                // gaialog_error("recv data failed...");
                throw "recv data failed...";
            }

            // gaialog_trace("real_recv {}_{} length:{}", m_to_party, m_from_party, move_in_str.length());
            system_clock::time_point end_time = system_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            if (secs.count() > 1000) {
                // gaialog_info("recv time used:{}", secs.count());
            }
        }

        statistics_recv(move_in_str.length());
    }

    channel::~channel() {
        if (m_client) {
            if (!m_client->flushed()) {
                bool flag = m_client->wait_flush(10000);
                if (!flag) {
                    // gaialog_error("send data my not flush...");
                }
            }
        }
        destroy_channel();
    }

    void channel::client_finish() {
        if (m_client) {
            m_client->finish();
            delete m_client;
            m_client = nullptr;
        }
    }

    void channel::send(const void* buf, uint64_t nbytes) {
        string str((char*)buf, nbytes);
        real_send(str);
    }

    void channel::send(std::string& str) {
        if (m_use_redis)
        {
            uint32_t str_size = str.size();
            send(&str_size, sizeof(uint32_t));
            uint32_t sent_size = 0;
            while (sent_size < str.size()) {
                uint32_t chunk_size = std::min(CHUNK_SIZE, str_size - sent_size);
                send(str.data() + sent_size, chunk_size);
                sent_size += chunk_size;
            }
        }else{
            real_send(str);
        }
    }

    void channel::send_with_compress(const void* buf, uint64_t nbytes) {
        std::string compress_str = get_lz4_compress_str((const char*)buf, nbytes);
        real_send(compress_str);
    }

    void channel::send_with_compress(const std::string& str) {
        return send_with_compress(str.data(), str.length());
    }

    void channel::recv(void* pbuf, uint64_t length) {
        std::string str;
        real_recv(str);

        // gaialog_assert_msg(str.length() == length, "exec Receiver net packet length error,need: {},get:{}", length, str.length());

        memcpy(pbuf, str.data(), length);
    }

    void channel::recv(std::string& str) {
        if (m_use_redis)
        {   
            uint32_t str_size = 0;
            recv(&str_size, sizeof(uint32_t));
            str.resize(str_size);
            uint32_t received_size = 0;
            while (received_size < str_size) {
                uint32_t chunk_size = std::min(CHUNK_SIZE, str_size - received_size);
                recv(str.data() + received_size, chunk_size);
                received_size += chunk_size;
            }
        }else{
             real_recv(str);
        }
    }

    void channel::recv_with_decompress(void* pbuf, uint64_t length) {
        std::string str;
        recv_with_decompress(str);

        // gaialog_assert_msg(str.length() == length, "exec Receiver net packet length error,need: {},get:{}", length, str.length());

        memcpy(pbuf, str.data(), length);
    }

    void channel::recv_with_decompress(std::string& str) {
        std::string compress_str;
        real_recv(compress_str);

        std::string plain_str = get_lz4_decompress_str(compress_str.data(), compress_str.length());
        str.swap(plain_str);
    }

    bool channel::flush() {
        for (int i = 0; i < 120; ++i) {
            bool need_retry = false;
            bool flag = m_client->wait_flush(10000, &need_retry);
            if (flag) {
                return true;
            }

            if (!need_retry) {
                return false;
            }

            // do retry
            // gaialog_warn("wait_flush failed ! retrying ......................:{}", i);
            // ugly code...
            m_client->finish();
            auto continue_status = m_client->get_continue_status();
            delete m_client;
            m_client = nullptr;
            // gaialog_info("disconnect remote server {} succeed", m_server_address);
            destroy_channel();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // gaialog_warn("reconnet to server with not ack data:{}", std::get<0>(continue_status).size());
            create_channel();
            flag = m_client->set_continue_status(continue_status);
            // gaialog_warn("flush packet to server again with set_continue_status:{}", flag);
        }

        return true;
    }

} // namespace gaianet
