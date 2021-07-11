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
#ifndef LIBBITCOIN_PROTOCOL_WEB_REPLY_HPP
#define LIBBITCOIN_PROTOCOL_WEB_REPLY_HPP

#include <string>
#include <unordered_map>
#include <bitcoin/protocol/define.hpp>
#include <bitcoin/protocol/web/http.hpp>
#include <bitcoin/protocol/web/protocol_status.hpp>

namespace libbitcoin {
namespace protocol {
namespace http {

class BCP_API http_reply
{
public:
    static std::string to_string(protocol_status status);

    static std::string generate(protocol_status status,
        const std::string& mime_type, size_t content_length, bool keep_alive);

    static std::string generate_upgrade(const std::string& key_response,
        const std::string& protocol);

    protocol_status status;
    string_map headers;
    std::string content;
};

} // namespace http
} // namespace protocol
} // namespace libbitcoin

#endif
