// This file Copyright © 2017-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <fmt/format.h>

#include <libutp/utp.h>

#include "transmission.h"

#include "peer-socket.h"
#include "net.h"
#include "session.h"

#define tr_logAddErrorIo(io, msg) tr_logAddError(msg, (io)->display_name())
#define tr_logAddWarnIo(io, msg) tr_logAddWarn(msg, (io)->display_name())
#define tr_logAddDebugIo(io, msg) tr_logAddDebug(msg, (io)->display_name())
#define tr_logAddTraceIo(io, msg) tr_logAddTrace(msg, (io)->display_name())

tr_peer_socket::tr_peer_socket(tr_session* session, tr_address const& address, tr_port port, tr_socket_t sock)
    : handle{ sock }
    , address_{ address }
    , port_{ port }
    , type_{ Type::TCP }
{
    TR_ASSERT(sock != TR_BAD_SOCKET);

    session->setSocketTOS(sock, address_.type);

    if (auto const& algo = session->peerCongestionAlgorithm(); !std::empty(algo))
    {
        tr_netSetCongestionControl(sock, algo.c_str());
    }

    tr_logAddTraceIo(this, fmt::format("socket (tcp) is {}", handle.tcp));
}

tr_peer_socket::tr_peer_socket(tr_address const& address, tr_port port, struct UTPSocket* const sock)
    : address_{ address }
    , port_{ port }
    , type_{ Type::UTP }
{
    TR_ASSERT(sock != nullptr);
    handle.utp = sock;

    tr_logAddTraceIo(this, fmt::format("socket (µTP) is {}", fmt::ptr(handle.utp)));
}

void tr_peer_socket::close(tr_session* session)
{
    if (is_tcp() && (handle.tcp != TR_BAD_SOCKET))
    {
        tr_netClose(session, handle.tcp);
    }
#ifdef WITH_UTP
    else if (is_utp())
    {
        utp_set_userdata(handle.utp, nullptr);
        utp_close(handle.utp);
    }
#endif

    type_ = Type::None;
    handle = {};
}

size_t tr_peer_socket::try_write(Buffer& buf, size_t max, tr_error** error) const
{
    if (max == size_t{})
    {
        return {};
    }

    if (is_tcp())
    {
        return buf.toSocket(handle.tcp, max, error);
    }

#ifdef WITH_UTP
    if (is_utp())
    {
        auto iov = buf.vecs(max);

        errno = 0;
        auto const n = utp_writev(handle.utp, reinterpret_cast<struct utp_iovec*>(std::data(iov)), std::size(iov));
        auto const error_code = errno;

        if (n > 0)
        {
            buf.drain(n);
            return static_cast<size_t>(n);
        }

        if (n < 0 && error_code != 0)
        {
            tr_error_set(error, error_code, tr_strerror(error_code));
        }
    }
#endif

    return {};
}

size_t tr_peer_socket::try_read(Buffer& buf, size_t max, tr_error** error) const
{
    if (max == size_t{})
    {
        return {};
    }

    if (is_tcp())
    {
        return buf.addSocket(handle.tcp, max, error);
    }

#ifdef WITH_UTP
    if (is_utp())
    {
        // utp_read_drained() notifies libutp that this read buffer is
        // empty.  It opens up the congestion window by sending an ACK
        // (soonish) if one was not going to be sent.
        if (std::empty(buf))
        {
            utp_read_drained(handle.utp);
        }
    }
#endif

    return {};
}

tr_peer_socket tr_netOpenPeerUTPSocket(
    tr_session* session,
    tr_address const& addr,
    tr_port port,
    bool /*client_is_seed*/,
    void* userdata)
{
    auto ret = tr_peer_socket{};

    if (session->utp_context != nullptr && addr.is_valid_for_peers(port))
    {
        auto const [ss, sslen] = addr.to_sockaddr(port);

        if (auto* const sock = utp_create_socket(session->utp_context); sock != nullptr)
        {
            utp_set_userdata(sock, userdata);

            if (utp_connect(sock, reinterpret_cast<sockaddr const*>(&ss), sslen) != -1)
            {
                ret = tr_peer_socket{ addr, port, sock };
            }
            else
            {
                utp_close(sock);
            }
        }
    }

    return ret;
}
