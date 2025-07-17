
#ifndef GAIA_MEMCHANNEL_H_
#define GAIA_MEMCHANNEL_H_
#pragma once

#include "ichannel.h"
constexpr const int macos_buf_size = 1<<30; // 1GB
namespace gaianet {

    namespace detail {
        struct SharedMemoryView;
    } // namespace detail

    class MemChannel final : public IChannel {
      public:
        MemChannel(uint32_t from_party, uint32_t to_party, const std::string& taskid, bool enable_cv = true, int buf_size = macos_buf_size);
        ~MemChannel();

        virtual void send(const void* buf, uint64_t nbytes) override;
        virtual void send(std::string& str) override;
        virtual void recv(std::string& str) override;
        virtual void recv(void* buf, uint64_t length) override;
        virtual bool flush() override;

      private:
        void recv_body(void* buf, uint64_t length);
        void attach();

      private:
        const int m_buf_size;
        const bool m_enable_cv;
        bool need_attach;
        detail::SharedMemoryView* m_send_buf;
        detail::SharedMemoryView* m_recv_buf;
        int m_send_fd;
        int m_recv_fd;
    };
} // namespace gaianet

#endif