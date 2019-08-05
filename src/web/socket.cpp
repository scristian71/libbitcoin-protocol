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
#include <bitcoin/protocol/web/socket.hpp>

 // TODO: include other headers.

namespace libbitcoin {
namespace protocol {
namespace http {

using namespace bc::protocol;
using namespace bc::system;
using namespace bc::system::chain;
using namespace boost::filesystem;
using namespace boost::iostreams;
using namespace boost::property_tree;
using namespace http;
using http_event = http::event;
using role = zmq::socket::role;

// Local class.
class task_sender
  : public manager::task
{
public:
    task_sender(connection_ptr connection, const std::string& data)
      : connection_(connection), data_(data)
    {
    }

    bool run()
    {
        if (!connection_ || connection_->closed())
            return false;

        auto narrow = [](int32_t& out, size_t in)
        {
            out = static_cast<int32_t>(in);
            return out >= 0 && out <= std::numeric_limits<int32_t>::max() &&
                static_cast<size_t>(out) == in;
        };

        int32_t data_size;
        if (!narrow(data_size, data_.size()))
            return false;

        if (!connection_->json_rpc())
        {
            LOG_VERBOSE(LOG_PROTOCOL_HTTP)
                << "Writing Websocket response: " << data_;
            return connection_->write(data_) == data_size;
        }

        http_reply reply;
        const auto response = reply.generate(protocol_status::ok, {},
            data_.size(), false) + data_;

        LOG_VERBOSE(LOG_PROTOCOL_HTTP)
            << "Writing JSON-RPC response: " << response;

        int32_t response_size;
        if (!narrow(response_size, response.size()))
            return false;

        return connection_->write(response) == response_size;
    }

    connection_ptr connection()
    {
        return connection_;
    }

private:
    connection_ptr connection_;
    const std::string data_;
};

// Local class.
//
// The purpose of this class is to handle previously received zmq
// responses and write them to the requesting websocket.
//
// The run() method is only called from send_query_responses on the
// web thread.  With this guarantee in mind, no locking of any state
// is required.
class query_response_task_sender
  : public socket::query_response_task
{
public:
    query_response_task_sender(uint32_t sequence, const data_chunk& data,
        const std::string& command, const socket::handler_map& handlers,
        const socket::handler_map& rpc_handlers,
        socket::connection_work_map& work,
        socket::query_correlation_map& correlations)
      : sequence_(sequence),
        data_(data),
        command_(command),
        handlers_(handlers),
        rpc_handlers_(rpc_handlers),
        work_(work),
        correlations_(correlations)
    {
    }

    bool run()
    {
        // Use internal sequence number to find connection and work id.
        auto correlation = correlations_.find(sequence_);
        if (correlation == correlations_.end())
        {
            // This will happen anytime the client disconnects before this handler
            // is called. We can safely discard the result here.
            LOG_DEBUG(LOG_PROTOCOL)
                << "Unmatched websocket query work item sequence: " << sequence_;
            return true;
        }

        auto connection = correlation->second.first;
        const auto id = correlation->second.second;
        correlations_.erase(correlation);

        // Use connection to locate connection state.
        auto it = work_.find(connection);
        if (it == work_.end())
        {
            LOG_ERROR(LOG_PROTOCOL)
                << "Query work completed for unknown connection";
            return true;
        }

        // Use work id to locate the query work item.
        auto& query_work_map = it->second;
        auto query_work = query_work_map.find(id);
        if (query_work == query_work_map.end())
        {
            // This can happen anytime the client disconnects before this
            // code is reached. We can safely discard the result here.
            LOG_DEBUG(LOG_PROTOCOL)
                << "Unmatched websocket query work id: " << id;
            return true;
        }

        const auto work = query_work->second;
        query_work_map.erase(query_work);

        BITCOIN_ASSERT(work.id == id);
        BITCOIN_ASSERT(work.connection == connection);
        BITCOIN_ASSERT(work.correlation_id == sequence_);

        auto write_error = [&work](system::code ec, uint32_t id, bool rpc)
        {
            const auto reply = rpc ? rpc::to_json(ec, id) : to_json(ec, id);
            work.connection->write(reply);
            return true;
        };

        data_source istream(data_);
        istream_reader source(istream);
        const auto ec = source.read_error_code();

        if (ec)
            return write_error(ec, id, connection->json_rpc());

        socket::handler_map::const_iterator handler;

        if (connection->json_rpc())
        {
            handler = rpc_handlers_.find(work.command);
            if (handler == rpc_handlers_.end())
                return write_error(system::error::not_implemented, id, true);
        }
        else
        {
            handler = handlers_.find(work.command);
            if (handler == handlers_.end())
                return write_error(system::error::not_implemented, id, false);
        }

        // Decode response and send query output to websocket client.
        // The websocket write is performed directly (running on the
        // websocket thread).
        handler->second.decode(source.read_bytes(), id, work.connection);
        return true;
    }

private:
    const uint32_t sequence_;
    const data_chunk data_;
    const std::string command_;
    const socket::handler_map& handlers_;
    const socket::handler_map& rpc_handlers_;
    socket::connection_work_map& work_;
    socket::query_correlation_map& correlations_;
};

// TODO: eliminate the use of weak and untyped pointer to pass self here.
// static
// Callback made internally via socket::poll on the web socket thread.
bool socket::handle_event(connection_ptr connection, http_event event,
    const void* data)
{
    switch (event)
    {
        case http_event::accepted:
        {
            // This connection is newly accepted and is either an HTTP
            // JSON-RPC connection, or an already upgraded websocket.
            // Returning false here will cause the service to stop
            // accepting new connections.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            instance->add_connection(connection);

            const auto connection_type = connection->json_rpc() ? "JSON-RPC" :
                "Websocket";
            LOG_DEBUG(LOG_PROTOCOL)
                << connection_type << " client connection established ["
                << connection << "] (" << instance->connection_count() << ")";
            break;
        }

        case http_event::json_rpc:
        {
            // Process new incoming user json_rpc request.  Returning
            // false here will cause this connection to be closed.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            BITCOIN_ASSERT(data != nullptr);
            const auto& request = *reinterpret_cast<const http_request*>(data);
            BITCOIN_ASSERT(request.json_rpc);

            // Use default-value get to avoid exceptions on invalid input.
            const auto id = request.json_tree.get<uint32_t>("id", 0);
            const auto method = request.json_tree.get<std::string>("method", "");

            if (request.json_tree.count("params") == 0)
            {
                http_reply reply;
                connection->write(reply.generate(
                    protocol_status::bad_request, {}, 0, false));
                return false;
            }

            std::vector<std::string> parameter_list;
            const auto child = request.json_tree.get_child("params");
            for (const auto& parameter: child)
                parameter_list.push_back(
                    parameter.second.get_value<std::string>());

            // TODO: Support full parameter lists?
            std::string parameters{};
            if (!parameter_list.empty())
                parameters = parameter_list[0];

            LOG_VERBOSE(LOG_PROTOCOL)
                << "method " << method << ", parameters " << parameters
                << ", id " << id;

            instance->notify_query_work(connection, method, id, parameters);
            break;
        }

        case http_event::websocket_frame:
        {
            // Process new incoming user websocket data. Returning false
            // will cause this connection to be closed.
            auto instance = static_cast<socket*>(connection->user_data());
            if (instance == nullptr)
                return false;

            BITCOIN_ASSERT(data != nullptr);
            auto message = reinterpret_cast<const websocket_message*>(data);

            ptree input_tree;
            if (!property_tree(input_tree,
                { message->data, message->data + message->size }))
            {
                http_reply reply;
                connection->write(reply.generate(
                    protocol_status::internal_server_error, {}, 0, false));
                return false;
            }

            // Use default value get to avoid exceptions on invalid input.
            const auto id = input_tree.get<uint32_t>("id", 0);
            const auto method = input_tree.get<std::string>("method", "");
            std::string parameters;

            const auto child = input_tree.get_child("params");
            std::vector<std::string> parameter_list;
            for (const auto& parameter: child)
                parameter_list.push_back(
                    parameter.second.get_value<std::string>());

            // TODO: Support full parameter lists?
            if (!parameter_list.empty())
                parameters = parameter_list[0];

            LOG_VERBOSE(LOG_PROTOCOL)
                << "method " << method << ", parameters " << parameters
                << ", id " << id;

            instance->notify_query_work(connection, method, id, parameters);
            break;
        }

        case http_event::closing:
        {
            // This connection is going away after this handling.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            instance->remove_connection(connection);

            const auto type = connection->json_rpc() ? "JSON-RPC" : "Websocket";
            LOG_DEBUG(LOG_PROTOCOL)
                << type << " client disconnected [" << connection << "] ("
                << instance->connection_count() << ")";
            break;
        }

        // No specific handling required for other events.
        case http_event::read:
        case http_event::error:
        case http_event::websocket_control_frame:
        default:
            break;
    }

    return true;
}

socket::socket(zmq::context& context, const protocol::settings& settings,
    bool secure)
  : worker(priority(settings.web_priority)),
    context_(context),
    secure_(secure),
    security_(secure ? "secure" : "public"),
    settings_(settings),
    sequence_(0),
    manager_(nullptr)
{
}

bool socket::start()
{
    if (!settings_.web_root.empty() && !exists(settings_.web_root))
    {
        LOG_ERROR(LOG_PROTOCOL)
            << "Configured HTTP root path '" << settings_.web_root
            << "' does not exist.";
        return false;
    }

    if (secure_)
    {
#ifdef WITH_MBEDTLS
        if (!settings_.web_ca_certificate.empty() &&
            !exists(settings_.web_server_certificate))
        {
            LOG_ERROR(LOG_PROTOCOL)
                << "Configured CA certificate '" << settings_.web_ca_certificate
                << "' does not exist.";
            return false;
        }

        if (!settings_.web_server_certificate.empty() &&
            !exists(settings_.web_server_certificate))
        {
            LOG_ERROR(LOG_PROTOCOL)
                << "Configured server certificate '"
                << settings_.web_server_certificate << "' does not exist.";
            return false;
        }

        if (!settings_.web_server_private_key.empty() &&
            !exists(settings_.web_server_private_key))
        {
            LOG_ERROR(LOG_PROTOCOL)
                << "Configured server private key '"
                << settings_.web_server_private_key << "' does not exist.";
            return false;
        }

        // If required secure parameters are empty, do not enable the service.
        if (settings_.web_server_certificate.empty() &&
            settings_.web_server_private_key.empty())
        {
            LOG_VERBOSE(LOG_PROTOCOL)
                << "Skipping start of secure websocket service "
                << "(no certificates configured)";
            return true;
        }
#else
        LOG_VERBOSE(LOG_PROTOCOL)
            << "Skipping start of secure websocket service "
            << "(not compiled with SSL support)";
        return true;
#endif
    }

    return zmq::worker::start();
}

void socket::queue_response(uint32_t sequence, const data_chunk& data,
    const std::string& command)
{
    auto task = std::make_shared<query_response_task_sender>(
        sequence, data, command, handlers_, rpc_handlers_, work_,
        correlations_);

    // Critical Section.
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(query_response_task_mutex_);
    query_response_tasks_.push_back(task);
    ///////////////////////////////////////////////////////////////////////////
}

bool socket::send_query_responses()
{
    query_response_task_list tasks;

    // Critical Section.
    ///////////////////////////////////////////////////////////////////////////
    query_response_task_mutex_.lock();
    query_response_tasks_.swap(tasks);
    query_response_task_mutex_.unlock();
    ///////////////////////////////////////////////////////////////////////////

    for (const auto task: tasks)
        if (!task->run())
            return false;

    return true;
}

void socket::handle_websockets()
{
    bind_options options;

    auto format_origins = [](const config::endpoint::list& endpoints)
    {
        manager::origin_list origins;
        origins.reserve(endpoints.size());
        for (const auto& endpoint: endpoints)
            origins.emplace_back(endpoint.to_string());

        return origins;
    };

    // This starts up the listener for the socket.
    manager_ = std::make_shared<manager>(secure_, &socket::handle_event,
        settings_.web_root, format_origins(settings_.web_origins));

    if (!manager_ || !manager_->initialize())
    {
        LOG_ERROR(LOG_PROTOCOL)
            << "Failed to initialize websocket manager";
        socket_started_.set_value(false);
        return;
    }

    if (secure_)
    {
        options.ssl_key = settings_.web_server_private_key;
        options.ssl_certificate = settings_.web_server_certificate;
        options.ssl_ca_certificate = settings_.web_ca_certificate;
    }

    options.user_data = static_cast<void*>(this);
    if (!manager_->bind(websocket_endpoint(), options))
    {
        socket_started_.set_value(false);
        return;
    }

    auto callback = [this]()
    {
        send_query_responses();
    };

    socket_started_.set_value(true);
    manager_->start(static_cast<manager::handler>(callback));
}

// NOTE: query_socket is the only service that should implement this
// by returning something other than nullptr.
//
// The reason it's needed is so that socket::notify_query_work (which
// is called from handle_event in the web thread via
// handle_websockets) can retrieve the zmq socket within the query
// socket service (created on the same websocket thread) in order to
// send incoming requests to the internally connected zmq
// query_service.  No other socket/service class requires this access.
const std::shared_ptr<zmq::socket> socket::service() const
{
    BITCOIN_ASSERT_MSG(false, "not implemented");
    return nullptr;
}

bool socket::start_websocket_handler()
{
    auto status = socket_started_.get_future();
    thread_ = std::make_shared<asio::thread>(&socket::handle_websockets, this);
    return status.get();
}

bool socket::stop_websocket_handler()
{
    BITCOIN_ASSERT(manager_);
    manager_->stop();
    thread_->join();
    return true;
}

size_t socket::connection_count() const
{
    return work_.size();
}

// Called by the websocket handling thread via handle_event.
void socket::add_connection(connection_ptr connection)
{
    BITCOIN_ASSERT(work_.find(connection) == work_.end());

    // Initialize a new query_work_map for this connection.
    work_[connection].clear();
}

// Called by the websocket handling thread via handle_event.
void socket::remove_connection(connection_ptr connection)
{
    if (work_.empty())
        return;

    const auto it = work_.find(connection);
    if (it != work_.end())
    {
        // Tearing down a connection is O(n) where n is the amount of
        // remaining outstanding queries.
        auto& query_work_map = it->second;
        for (const auto& query_work: query_work_map)
        {
            const auto correlation = correlations_.find(
                query_work.second.correlation_id);

            if (correlation != correlations_.end())
                correlations_.erase(correlation);
        }

        // Clear the query_work_map for this connection before removal.
        query_work_map.clear();
        work_.erase(it);
    }
}

// Called by the websocket handling thread via handle_event.
//
// Errors write directly on the connection since this is called from
// the event_handler, which is called on the websocket thread.
void socket::notify_query_work(connection_ptr connection,
    const std::string& method, uint32_t id, const std::string& parameters)
{
    const auto rpc = connection->json_rpc();
    const auto send_error_reply = [=](protocol_status status, const code& ec)
    {
        http_reply reply;
        const auto error = (rpc ? rpc::to_json(ec, id) : to_json(ec, id));
        const auto response = reply.generate(status, {}, error.size(), false);
        LOG_VERBOSE(LOG_PROTOCOL) << response + error;
        connection->write(response + error);
    };

    // This occurs when a websocket/rpc request is pointed at a web
    // endpoint other than the query service.
    if (handlers_.empty() || rpc_handlers_.empty())
    {
        LOG_VERBOSE(LOG_PROTOCOL)
            << "No handlers for methods. Likely incorrect endpoint addressed.";
        return send_error_reply(protocol_status::service_unavailable,
            system::error::http_invalid_request);
    }

    handler_map::const_iterator handler;

    auto handler_not_found = [=](const std::string& method, bool rpc)
    {
        LOG_VERBOSE(LOG_PROTOCOL)
            << (rpc ? "JSON-RPC" : "Websocket") << " method" << method
            << " not found";
        return send_error_reply(protocol_status::not_found,
            system::error::http_method_not_found);
    };

    if (rpc)
    {
        handler = rpc_handlers_.find(method);
        if (handler == rpc_handlers_.end())
            return handler_not_found(method, rpc);
    }
    else
    {
        handler = handlers_.find(method);
        if (handler == handlers_.end())
            return handler_not_found(method, rpc);
    }

    auto it = work_.find(connection);
    if (it == work_.end())
    {
        LOG_ERROR(LOG_PROTOCOL)
            << "Query work provided for unknown connection " << connection;
        return;
    }

    auto& query_work_map = it->second;
    if (query_work_map.find(id) != query_work_map.end())
    {
        send_error_reply(protocol_status::internal_server_error,
            system::error::http_internal_error);
        return;
    }

    query_work_map.emplace(id,
        query_work_item{ id, sequence_, connection, method, parameters });

    // Encode request based on query work and send to query_websocket.
    zmq::message request;
    if (!handler->second.encode(request, handler->second.command, parameters,
        sequence_))
    {
        LOG_WARNING(LOG_PROTOCOL)
            << "Encoding command " << handler->second.command
            << " with parameters " << parameters << " failed.";
        send_error_reply(protocol_status::bad_request,
            system::error::http_invalid_request);
        return;
    }

    // While each connection has its own id map (meaning correlation ids passed
    // from the web client are unique on a per connection basis, potentially
    // utilizing the full range available), we need an internal mapping that
    // will allow us to correlate each zmq request/response pair with the
    // connection and original id number that originated it. The client never
    // sees this sequence_ value.
    correlations_[sequence_++] = { connection, id };

    const auto ec = service()->send(request);

    if (ec)
    {
        send_error_reply(protocol_status::internal_server_error,
            system::error::http_internal_error);
        return;
    }
}

// Sends json strings to the specified web or json_rpc socket (does nothing if
// neither).
void socket::send(connection_ptr connection, const std::string& json)
{
    if (!connection || connection->closed() ||
        (!connection->websocket() && !connection->json_rpc()))
        return;

    // By using a task_sender via the manager's execute method, we guarantee
    // that the write is performed on the manager's websocket thread (at the
    // expense of copied json send and response payloads).
    manager_->execute(std::make_shared<task_sender>(connection, json));
}

// Sends json strings to all connected web and json_rpc sockets.
void socket::broadcast(const std::string& json)
{
    auto sender = [this, &json](std::pair<connection_ptr, query_work_map> entry)
    {
        send(entry.first, json);
    };

    std::for_each(work_.begin(), work_.end(), sender);
}

void socket::set_default_page_data(const std::string& data)
{
    manager_->set_default_page_data(data);
}

} // namespace http
} // namespace protocol
} // namespace libbitcoin
