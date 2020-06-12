#include <eosio/amqp_trace_plugin/amqp_trace_plugin.hpp>
#include <eosio/amqp_trace_plugin/amqp_publish_handler.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>

#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/thread_utils.hpp>

#include <boost/signals2/connection.hpp>

namespace {

static appbase::abstract_plugin& amqp_trace_plugin_ = appbase::app().register_plugin<eosio::amqp_trace_plugin>();

} // anonymous

namespace eosio {

using boost::signals2::scoped_connection;

struct amqp_trace_plugin_impl : std::enable_shared_from_this<amqp_trace_plugin_impl> {

   // use thread pool even though only one thread currently since it provides simple interface for ioc
   std::optional<eosio::chain::named_thread_pool> thread_pool;
   std::optional<amqp_publish> amqp_trace;
   std::optional<scoped_connection> applied_transaction_connection;

   std::string amqp_trace_address;
   std::string amqp_trace_exchange;

public:

   // called from any thread
   void publish_error( std::string tid, int64_t error_code, std::string error_message ) {
      try {
         transaction_trace_msg msg{transaction_trace_exception{error_code}};
         msg.get<transaction_trace_exception>().error_message = std::move( error_message );
         auto buf = fc::raw::pack( msg );
         boost::asio::post( thread_pool->get_executor(), [my=shared_from_this(), buf=std::move(buf), tid=std::move(tid)]() {
            my->amqp_trace->publish( my->amqp_trace_exchange, tid, buf.data(), buf.size() );
         } );
      } FC_LOG_AND_DROP()
   }

   // called on application thread
   void on_applied_transaction(const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& t) {
      try {
         boost::asio::post( thread_pool->get_executor(), [my=shared_from_this(), trace, t]() {
            my->publish_result( t, trace );
         } );
      } FC_LOG_AND_DROP()
   }

private:

   // called from amqp thread
   void publish_result( const chain::packed_transaction_ptr& trx, const chain::transaction_trace_ptr& trace ) {
      try {
         if( !trace->except ) {
            dlog( "chain accepted transaction, bcast ${id}", ("id", trace->id) );
         } else {
            dlog( "trace except : ${m}", ("m", trace->except->to_string()) );
         }
         fc::unsigned_int which = transaction_trace_msg::tag<chain::transaction_trace>::value;
         uint32_t payload_size = fc::raw::pack_size( which );
         payload_size += fc::raw::pack_size( *trace );
         std::vector<char> buf( payload_size );
         fc::datastream<char*> ds( buf.data(), payload_size );
         fc::raw::pack( ds, which );
         fc::raw::pack( ds, *trace );
         amqp_trace->publish( amqp_trace_exchange, trx->id(), buf.data(), buf.size() );
      } FC_LOG_AND_DROP()
   }

};

amqp_trace_plugin::amqp_trace_plugin()
: my(std::make_shared<amqp_trace_plugin_impl>()) {}

amqp_trace_plugin::~amqp_trace_plugin() {}

void amqp_trace_plugin::publish_error( std::string tid, int64_t error_code, std::string error_message ) {
   my->publish_error( std::move(tid), error_code, std::move(error_message) );
}

void amqp_trace_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto op = cfg.add_options();
   op("amqp-trace-address", bpo::value<std::string>(),
      "AMQP address: Format: amqp://USER:PASSWORD@ADDRESS:PORT\n"
      "Will consume from 'trx' queue and publish to 'trace' queue.");
   op("amqp-trace-exchange", bpo::value<std::string>()->default_value(""),
      "Existing AMQP exchange to send transaction trace messages.");
}

void amqp_trace_plugin::plugin_initialize(const variables_map& options) {
   try {
      if( options.count("amqp-trace-address") )
         my->amqp_trace_address = options.at("amqp-trace-address").as<std::string>();
      my->amqp_trace_exchange = options.at("amqp-trace-exchange").as<std::string>();
   }
   FC_LOG_AND_RETHROW()
}

void amqp_trace_plugin::plugin_startup() {
   handle_sighup();
   try {

      if( my->amqp_trace_address.empty() ) return;

      ilog( "Starting amqp_trace_plugin" );
      my->thread_pool.emplace( "amqp_t", 1 );

      my->amqp_trace.emplace( my->thread_pool->get_executor(), my->amqp_trace_address, "trace", [](const std::string& err) {
         elog( "amqp error: ${e}", ("e", err) );
         app().quit();
      });

      auto chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT( chain_plug, chain::missing_chain_plugin_exception, "chain_plugin required" );

      my->applied_transaction_connection.emplace(
            chain_plug->chain().applied_transaction.connect(
                  [me=my]( std::tuple<const chain::transaction_trace_ptr&, const chain::packed_transaction_ptr&> t ) {
                     me->on_applied_transaction( std::get<0>( t ), std::get<1>( t ) );
                  } ) );

   } catch( ... ) {
      // always want plugin_shutdown even on exception
      plugin_shutdown();
      throw;
   }
}

void amqp_trace_plugin::plugin_shutdown() {
   try {
      dlog( "shutdown.." );

      my->applied_transaction_connection.reset();
      if( my->thread_pool ) {
         my->thread_pool->stop();
      }

      dlog( "exit amqp_trace_plugin" );
   }
   FC_CAPTURE_AND_RETHROW()
}

void amqp_trace_plugin::handle_sighup() {
}

} // namespace eosio
