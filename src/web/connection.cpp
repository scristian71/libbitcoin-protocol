/**
 * Copyright (c) 2011-2019 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/protocol/web/connection.hpp>

 // TODO: include other headers.

namespace libbitcoin {
namespace protocol {
namespace http {

using namespace bc::system;

static constexpr size_t maximum_read_length = 1024;
static constexpr size_t high_water_mark = 2 * 1024 * 1024;

connection::connection()
  : connection(0, {})
{
}

connection::connection(sock_t connection, const sockaddr_in& address)
  : user_data_(nullptr),
    state_(connection_state::unknown),
    socket_(connection),
    address_(address),
    last_active_(system::asio::steady_clock::now()),
    ssl_context_{},
    websocket_(false),
    json_rpc_(false),
    file_transfer_{},
    websocket_transfer_{},
    bytes_read_(0)
{
    write_buffer_.reserve(high_water_mark);
}

connection::~connection()
{
    if (!closed())
        close();
}

connection_state connection::state() const
{
    return state_;
}

void connection::set_state(connection_state state)
{
    state_ = state;
}

void connection::set_socket_non_blocking()
{
#ifdef _MSC_VER
    ULONG non_blocking = 1;
    ioctlsocket(socket_, FIONBIO, &non_blocking);
#else
    fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFD) | O_NONBLOCK);
#endif
}

sockaddr_in connection::address() const
{
    return address_;
}

bool connection::reuse_address() const
{
    static constexpr uint32_t opt = 1;

    // reinterpret_cast required for Win32, otherwise nop.
    return setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&opt), sizeof(opt)) != -1;
}

bool connection::closed() const
{
    return state_ == connection_state::closed;
}

int32_t connection::read()
{
#ifdef WIN32
    // reinterpret_cast required for Win32, otherwise nop.
    auto data = reinterpret_cast<char*>(read_buffer_.data());
#else
    auto data = read_buffer_.data();
#endif

#ifdef WITH_MBEDTLS
    bytes_read_ = (ssl_context_.enabled ?
        mbedtls_ssl_read(&ssl_context_.context, data, maximum_read_length) :
            recv(socket_, data, maximum_read_length, 0));
#else
    bytes_read_ = recv(socket_, data, maximum_read_length, 0);
#endif
    return bytes_read_;
}

int32_t connection::read_length()
{
    return bytes_read_;
}

http::read_buffer& connection::read_buffer()
{
    return read_buffer_;
}

data_chunk& connection::write_buffer()
{
    return write_buffer_;
}

int32_t connection::unbuffered_write(const data_chunk& buffer)
{
    return unbuffered_write(buffer.data(), buffer.size());
}

int32_t connection::unbuffered_write(const std::string& buffer)
{
    const auto data = reinterpret_cast<const uint8_t*>(buffer.data());
    return unbuffered_write(data, buffer.size());
}

int32_t connection::unbuffered_write(const uint8_t* data, size_t length)
{
    const auto plaintext_write = [this](const uint8_t* data, size_t length)
    {
        // BUGBUG: must set errno for return error handling.
        if (length > static_cast<size_t>(max_int32))
            return -1;

#ifdef _MSC_VER
        return send(socket_, reinterpret_cast<const char*>(data),
            static_cast<int32_t>(length), 0);
#else
        return static_cast<int32_t>(send(socket_, data, length, 0));
#endif
    };

#ifdef WITH_MBEDTLS
    const auto ssl_write = [this](const uint8_t* data, size_t length)
    {
        int32_t value = mbedtls_ssl_write(&ssl_context_.context, data, length);
        return mbedtls_would_block(value) ? WOULD_BLOCK : value;
    };

    auto writer = ssl_context_.enabled ?
        static_cast<write_method>(ssl_write) :
        static_cast<write_method>(plaintext_write);
#else
    auto writer = static_cast<write_method>(plaintext_write);
#endif

    auto remaining = length;
    auto position = data;

    do
    {
        const auto written = writer(position, remaining);

        if (written < 0)
        {
            const auto error = last_error();
            if (!would_block(error))
            {
                LOG_WARNING(LOG_PROTOCOL_HTTP)
                    << "Unbuffered write failed. requested " << remaining
                    << " and wrote " << written << ": " << error_string();
                return written;
            }

            // BUGBUG: non-terminating loop, or does would_block prevent?
            continue;
        }

        position += written;
        remaining -= written;

    } while (remaining != 0);

    // TODO: isn't this always length?
    return static_cast<int32_t>(position - data);
}

int32_t connection::write(const data_chunk& buffer)
{
    return write(buffer.data(), buffer.size());
}

int32_t connection::write(const std::string& buffer)
{
    const auto data = reinterpret_cast<const uint8_t*>(buffer.data());
    return write(data, buffer.size());
}

// If high water would be exceeded new messages are silently dropped.
int32_t connection::write(const uint8_t* data, size_t length)
{
    // BUGBUG: must set errno for return error handling.
    if (length > static_cast<size_t>(max_int32))
        return -1;

    const auto header = websocket_ ? websocket_frame::to_header(length,
        websocket_op::text) : data_chunk{};
    const auto buffer_size = write_buffer_.size() + header.size() + length;

    if (buffer_size > high_water_mark)
    {
        LOG_VERBOSE(LOG_PROTOCOL_HTTP)
            << "High water exceeded, " << length  << "byte message dropped.";
        return static_cast<int32_t>(length);
    }

    // TODO: this is very inefficient, use circular buffer.
    // Buffer header and data for future writes (called from poll).
    write_buffer_.insert(write_buffer_.end(), header.begin(), header.end());
    write_buffer_.insert(write_buffer_.end(), data, data + length);
    return static_cast<int32_t>(length);
}

void connection::close()
{
    if (state_ == connection_state::closed)
        return;

#ifdef WITH_MBEDTLS
    if (ssl_context_.enabled)
    {
        if (state_ != connection_state::listening)
            mbedtls_ssl_free(&ssl_context_.context);

        mbedtls_pk_free(&ssl_context_.key);
        mbedtls_x509_crt_free(&ssl_context_.certificate);
        mbedtls_x509_crt_free(&ssl_context_.ca_certificate);
        mbedtls_ssl_config_free(&ssl_context_.configuration);
        ssl_context_.enabled = false;
    }
#endif

    CLOSE_SOCKET(socket_);
    state_ = connection_state::closed;
    LOG_VERBOSE(LOG_PROTOCOL_HTTP)
        << "Closed socket " << this;
}

sock_t& connection::socket()
{
    return socket_;
}

http::ssl& connection::ssl_context()
{
    return ssl_context_;
}

bool connection::ssl_enabled() const
{
    return ssl_context_.enabled;
}

bool connection::websocket() const
{
    return websocket_;
}

void connection::set_websocket(bool websocket)
{
    websocket_ = websocket;
}

const std::string& connection::uri() const
{
    return uri_;
}

void connection::set_uri(const std::string& uri)
{
    uri_ = uri;
}

bool connection::json_rpc() const
{
    return json_rpc_;
}

void connection::set_json_rpc(bool json_rpc)
{
    json_rpc_ = json_rpc;
}

void* connection::user_data()
{
    return user_data_;
}

void connection::set_user_data(void* user_data)
{
    user_data_ = user_data;
}

http::file_transfer& connection::file_transfer()
{
    return file_transfer_;
}

http::websocket_transfer& connection::websocket_transfer()
{
    return websocket_transfer_;
}

bool connection::operator==(const connection& other)
{
    return user_data_ == other.user_data_ && socket_ == other.socket_;
}

} // namespace http
} // namespace protocol
} // namespace libbitcoin
