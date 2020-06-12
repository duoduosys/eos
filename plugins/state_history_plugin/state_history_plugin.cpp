#include <eosio/chain/config.hpp>
#include <eosio/state_history/log.hpp>
#include <eosio/state_history/serialization.hpp>
#include <eosio/state_history_plugin/state_history_plugin.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/signals2/connection.hpp>

using tcp    = boost::asio::ip::tcp;
namespace ws = boost::beast::websocket;

extern const char* const state_history_plugin_abi;

namespace eosio {
using namespace chain;
using namespace state_history;
using boost::signals2::scoped_connection;

static appbase::abstract_plugin& _state_history_plugin = app().register_plugin<state_history_plugin>();

template <typename F>
auto catch_and_log(F f) {
   try {
      return f();
   } catch (const fc::exception& e) {
      elog("${e}", ("e", e.to_detail_string()));
   } catch (const std::exception& e) {
      elog("${e}", ("e", e.what()));
   } catch (...) {
      elog("unknown exception");
   }
}

struct state_history_plugin_impl : std::enable_shared_from_this<state_history_plugin_impl> {
   chain_plugin*                                              chain_plug = nullptr;
   fc::optional<state_history_traces_log>                     trace_log;
   fc::optional<state_history_chain_state_log>                chain_state_log;
   bool                                                       stopping = false;
   fc::optional<scoped_connection>                            applied_transaction_connection;
   fc::optional<scoped_connection>                            block_start_connection;
   fc::optional<scoped_connection>                            accepted_block_connection;
   string                                                     endpoint_address = "0.0.0.0";
   uint16_t                                                   endpoint_port    = 8080;
   std::unique_ptr<tcp::acceptor>                             acceptor;

   void get_block(uint32_t block_num, fc::optional<bytes>& result) {
      chain::signed_block_ptr p;
      try {
         p = chain_plug->chain().fetch_block_by_number(block_num);
      } catch (...) {
         return;
      }
      if (p) {
         auto v0 = p->to_signed_block_v0();
         if (v0)
            result = fc::raw::pack(*v0);
      }
   }

   fc::optional<chain::block_id_type> get_block_id(uint32_t block_num) {
      if (trace_log && block_num >= trace_log->begin_block() && block_num < trace_log->end_block())
         return trace_log->get_block_id(block_num);
      if (chain_state_log && block_num >= chain_state_log->begin_block() && block_num < chain_state_log->end_block())
         return chain_state_log->get_block_id(block_num);
      try {
         auto block = chain_plug->chain().fetch_block_by_number(block_num);
         if (block)
            return block->calculate_id();
      } catch (...) {
      }
      return {};
   }

   struct session : std::enable_shared_from_this<session> {
      std::shared_ptr<state_history_plugin_impl> plugin;
      std::unique_ptr<ws::stream<tcp::socket>>   socket_stream;
      bool                                       sending  = false;
      bool                                       sent_abi = false;
      std::vector<std::vector<char>>             send_queue;
      fc::optional<get_blocks_request_v0>        current_request;
      bool                                       need_to_send_update = false;

      session(std::shared_ptr<state_history_plugin_impl> plugin)
          : plugin(std::move(plugin)) {}

      void start(tcp::socket socket) {
         ilog("incoming connection");
         socket_stream = std::make_unique<ws::stream<tcp::socket>>(std::move(socket));
         socket_stream->binary(true);
         socket_stream->next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
         socket_stream->next_layer().set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
         socket_stream->next_layer().set_option(boost::asio::socket_base::receive_buffer_size(1024 * 1024));
         socket_stream->async_accept([self = shared_from_this()](boost::system::error_code ec) {
            self->callback(ec, "async_accept", [self] {
               self->start_read();
               self->send(state_history_plugin_abi);
            });
         });
      }

      void start_read() {
         auto in_buffer = std::make_shared<boost::beast::flat_buffer>();
         socket_stream->async_read(
             *in_buffer, [self = shared_from_this(), in_buffer](boost::system::error_code ec, size_t) {
                self->callback(ec, "async_read", [self, in_buffer] {
                   auto d = boost::asio::buffer_cast<char const*>(boost::beast::buffers_front(in_buffer->data()));
                   auto s = boost::asio::buffer_size(in_buffer->data());
                   fc::datastream<const char*> ds(d, s);
                   state_request               req;
                   fc::raw::unpack(ds, req);
                   req.visit(*self);
                   self->start_read();
                });
             });
      }

      void send(const char* s) {
         send_queue.push_back({s, s + strlen(s)});
         send();
      }

      template <typename T>
      void send(T obj) {
         send_queue.push_back(fc::raw::pack(state_result{std::move(obj)}));
         send();
      }

      void send() {
         if (sending)
            return;
         if (send_queue.empty())
            return send_update();
         sending = true;
         socket_stream->binary(sent_abi);
         sent_abi = true;
         socket_stream->async_write( //
             boost::asio::buffer(send_queue[0]),
             [self = shared_from_this()](boost::system::error_code ec, size_t) {
                self->callback(ec, "async_write", [self] {
                   self->send_queue.erase(self->send_queue.begin());
                   self->sending = false;
                   self->send();
                });
             });
      }

      using result_type = void;
      void operator()(get_status_request_v0&) {
         auto&                chain = plugin->chain_plug->chain();
         get_status_result_v0 result;
         result.head              = {chain.head_block_num(), chain.head_block_id()};
         result.last_irreversible = {chain.last_irreversible_block_num(), chain.last_irreversible_block_id()};
         result.chain_id          = chain.get_chain_id();
         if (plugin->trace_log) {
            result.trace_begin_block = plugin->trace_log->begin_block();
            result.trace_end_block   = plugin->trace_log->end_block();
         }
         if (plugin->chain_state_log) {
            result.chain_state_begin_block = plugin->chain_state_log->begin_block();
            result.chain_state_end_block   = plugin->chain_state_log->end_block();
         }
         send(std::move(result));
      }

      void operator()(get_blocks_request_v0& req) {
         for (auto& cp : req.have_positions) {
            if (req.start_block_num <= cp.block_num)
               continue;
            auto id = plugin->get_block_id(cp.block_num);
            if (!id || *id != cp.block_id)
               req.start_block_num = std::min(req.start_block_num, cp.block_num);
         }
         req.have_positions.clear();
         current_request = req;
         send_update(true);
      }

      void operator()(get_blocks_ack_request_v0& req) {
         if (!current_request)
            return;
         current_request->max_messages_in_flight += req.num_messages;
         send_update();
      }

      void send_update(get_blocks_result_v0 result) {
         need_to_send_update = true;
         if (!send_queue.empty() || !current_request || !current_request->max_messages_in_flight)
            return;
         auto& chain = plugin->chain_plug->chain();
         result.last_irreversible = {chain.last_irreversible_block_num(), chain.last_irreversible_block_id()};
         uint32_t current =
               current_request->irreversible_only ? result.last_irreversible.block_num : result.head.block_num;
         if (current_request->start_block_num <= current &&
             current_request->start_block_num < current_request->end_block_num) {
            auto block_id = plugin->get_block_id(current_request->start_block_num);
            if (block_id) {
               result.this_block  = block_position{current_request->start_block_num, *block_id};
               auto prev_block_id = plugin->get_block_id(current_request->start_block_num - 1);
               if (prev_block_id)
                  result.prev_block = block_position{current_request->start_block_num - 1, *prev_block_id};
               if (current_request->fetch_block)
                  plugin->get_block(current_request->start_block_num, result.block);
               if (current_request->fetch_traces && plugin->trace_log) {
                  result.traces = plugin->trace_log->get_log_entry(current_request->start_block_num);
               }
               if (current_request->fetch_deltas && plugin->chain_state_log) {
                  result.deltas = plugin->chain_state_log->get_log_entry(current_request->start_block_num);
               }
            }
            ++current_request->start_block_num;
         }
         send(std::move(result));
         --current_request->max_messages_in_flight;
         need_to_send_update = current_request->start_block_num <= current &&
                               current_request->start_block_num < current_request->end_block_num;
      }

      void send_update(const block_state_ptr& block_state) {
         need_to_send_update = true;
         if (!send_queue.empty() || !current_request || !current_request->max_messages_in_flight)
            return;
         get_blocks_result_v0 result;
         result.head = {block_state->block_num, block_state->id};
         send_update(std::move(result));
      }

      void send_update(bool changed = false) {
         if (changed)
            need_to_send_update = true;
         if (!send_queue.empty() || !need_to_send_update || !current_request ||
             !current_request->max_messages_in_flight)
            return;
         auto& chain = plugin->chain_plug->chain();
         get_blocks_result_v0 result;
         result.head = {chain.head_block_num(), chain.head_block_id()};
         send_update(std::move(result));
      }

      template <typename F>
      void catch_and_close(F f) {
         try {
            f();
         } catch (const fc::exception& e) {
            elog("${e}", ("e", e.to_detail_string()));
            close();
         } catch (const std::exception& e) {
            elog("${e}", ("e", e.what()));
            close();
         } catch (...) {
            elog("unknown exception");
            close();
         }
      }

      template <typename F>
      void callback(boost::system::error_code ec, const char* what, F f) {
         app().post( priority::medium, [=]() {
            if( plugin->stopping )
               return;
            if( ec )
               return on_fail( ec, what );
            catch_and_close( f );
         } );
      }

      void on_fail(boost::system::error_code ec, const char* what) {
         try {
            elog("${w}: ${m}", ("w", what)("m", ec.message()));
            close();
         } catch (...) {
            elog("uncaught exception on close");
         }
      }

      void close() {
         socket_stream->next_layer().close();
         plugin->sessions.erase(this);
      }
   };
   std::map<session*, std::shared_ptr<session>> sessions;

   void listen() {
      boost::system::error_code ec;

      auto address  = boost::asio::ip::make_address(endpoint_address);
      auto endpoint = tcp::endpoint{address, endpoint_port};
      acceptor      = std::make_unique<tcp::acceptor>(app().get_io_service());

      auto check_ec = [&](const char* what) {
         if (!ec)
            return;
         elog("${w}: ${m}", ("w", what)("m", ec.message()));
         EOS_ASSERT(false, plugin_exception, "unable to open listen socket");
      };

      acceptor->open(endpoint.protocol(), ec);
      check_ec("open");
      acceptor->set_option(boost::asio::socket_base::reuse_address(true));
      acceptor->bind(endpoint, ec);
      check_ec("bind");
      acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
      check_ec("listen");
      do_accept();
   }

   void do_accept() {
      auto socket = std::make_shared<tcp::socket>(app().get_io_service());
      acceptor->async_accept(*socket, [self = shared_from_this(), socket, this](const boost::system::error_code& ec) {
         if (stopping)
            return;
         if (ec) {
            if (ec == boost::system::errc::too_many_files_open)
               catch_and_log([&] { do_accept(); });
            return;
         }
         catch_and_log([&] {
            auto s            = std::make_shared<session>(self);
            sessions[s.get()] = s;
            s->start(std::move(*socket));
         });
         catch_and_log([&] { do_accept(); });
      });
   }

   void on_applied_transaction(const transaction_trace_ptr& p, const packed_transaction_ptr& t) {
      if (trace_log)
         trace_log->add_transaction(p, t);
   }

   void on_accepted_block(const block_state_ptr& block_state) {
      if (trace_log)
         trace_log->store(chain_plug->chain().db(), block_state);
      if (chain_state_log)
         chain_state_log->store(chain_plug->chain().db(), block_state);
      for (auto& s : sessions) {
         auto& p = s.second;
         if (p) {
            if (p->current_request && block_state->block_num < p->current_request->start_block_num)
               p->current_request->start_block_num = block_state->block_num;
            p->send_update(block_state);
         }
      }
   }

   void on_block_start(uint32_t block_num) {
      if (trace_log)
         trace_log->block_start(block_num);
   }

};   // state_history_plugin_impl

state_history_plugin::state_history_plugin()
    : my(std::make_shared<state_history_plugin_impl>()) {}

state_history_plugin::~state_history_plugin() {}

void state_history_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto options = cfg.add_options();
   options("state-history-dir", bpo::value<bfs::path>()->default_value("state-history"),
           "the location of the state-history directory (absolute path or relative to application data dir)");
   cli.add_options()("delete-state-history", bpo::bool_switch()->default_value(false), "clear state history files");
   options("trace-history", bpo::bool_switch()->default_value(false), "enable trace history");
   options("chain-state-history", bpo::bool_switch()->default_value(false), "enable chain state history");
   options("state-history-endpoint", bpo::value<string>()->default_value("127.0.0.1:8080"),
           "the endpoint upon which to listen for incoming connections. Caution: only expose this port to "
           "your internal network.");
   options("trace-history-debug-mode", bpo::bool_switch()->default_value(false),
           "enable debug mode for trace history");
}

void state_history_plugin::plugin_initialize(const variables_map& options) {
   try {
      EOS_ASSERT(options.at("disable-replay-opts").as<bool>(), plugin_exception,
                 "state_history_plugin requires --disable-replay-opts");

      my->chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT(my->chain_plug, chain::missing_chain_plugin_exception, "");
      auto& chain = my->chain_plug->chain();
      my->applied_transaction_connection.emplace(
          chain.applied_transaction.connect([&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
             my->on_applied_transaction(std::get<0>(t), std::get<1>(t));
          }));
      my->accepted_block_connection.emplace(
          chain.accepted_block.connect([&](const block_state_ptr& p) { my->on_accepted_block(p); }));
      my->block_start_connection.emplace(
          chain.block_start.connect([&](uint32_t block_num) { my->on_block_start(block_num); }));

      auto                    dir_option = options.at("state-history-dir").as<bfs::path>();
      boost::filesystem::path state_history_dir;
      if (dir_option.is_relative())
         state_history_dir = app().data_dir() / dir_option;
      else
         state_history_dir = dir_option;

      auto ip_port         = options.at("state-history-endpoint").as<string>();
      auto port            = ip_port.substr(ip_port.find(':') + 1, ip_port.size());
      auto host            = ip_port.substr(0, ip_port.find(':'));
      my->endpoint_address = host;
      my->endpoint_port    = std::stoi(port);
      idump((ip_port)(host)(port));

      if (options.at("delete-state-history").as<bool>()) {
         ilog("Deleting state history");
         boost::filesystem::remove_all(state_history_dir);
      }
      boost::filesystem::create_directories(state_history_dir);

      if (options.at("trace-history").as<bool>()) {
         my->trace_log.emplace(state_history_dir);
         if (options.at("trace-history-debug-mode").as<bool>()) 
            my->trace_log->trace_debug_mode = true;                      
      }

      if (options.at("chain-state-history").as<bool>())
         my->chain_state_log.emplace(state_history_dir);
   }
   FC_LOG_AND_RETHROW()
} // state_history_plugin::plugin_initialize

void state_history_plugin::plugin_startup() { my->listen(); }

void state_history_plugin::plugin_shutdown() {
   my->applied_transaction_connection.reset();
   my->accepted_block_connection.reset();
   my->block_start_connection.reset();
   while (!my->sessions.empty())
      my->sessions.begin()->second->close();
   my->stopping = true;
}

} // namespace eosio
