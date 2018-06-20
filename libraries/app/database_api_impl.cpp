/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/app/database_api_impl.hpp>
#include <graphene/chain/get_config.hpp>
#include <graphene/chain/contract_table_objects.hpp>

#include <fc/bloom_filter.hpp>
#include <fc/smart_ref_impl.hpp>

#include <fc/crypto/hex.hpp>

#include <boost/range/iterator_range.hpp>
#include <boost/rational.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/algorithm/string.hpp>

#include <cctype>

#include <cfenv>
#include <iostream>


#define GET_REQUIRED_FEES_MAX_RECURSION 4

namespace graphene { namespace app {
using namespace boost;

template<class T>
optional<T> maybe_id( const string& name_or_id )
{
   if( std::isdigit( name_or_id.front() ) )
   {
      try
      {
         return fc::variant(name_or_id).as<T>();
      }
      catch (const fc::exception&)
      {
      }
   }
   return optional<T>();
}
//////////////////////////////////////////////////////////////////////
//                                                                  //
// Constructors                                                     //
//                                                                  //
//////////////////////////////////////////////////////////////////////
database_api_impl::database_api_impl( graphene::chain::database& db ):_db(db)
{
   _new_connection = _db.new_objects.connect([this](const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts) {
                                on_objects_new(ids, impacted_accounts);
                                });
   _change_connection = _db.changed_objects.connect([this](const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts) {
                                on_objects_changed(ids, impacted_accounts);
                                });
   _removed_connection = _db.removed_objects.connect([this](const vector<object_id_type>& ids, const vector<const object*>& objs, const flat_set<account_id_type>& impacted_accounts) {
                                on_objects_removed(ids, objs, impacted_accounts);
                                });
   _applied_block_connection = _db.applied_block.connect([this](const signed_block&){ on_applied_block(); });

   _pending_trx_connection = _db.on_pending_transaction.connect([this](const signed_transaction& trx ){
                         if( _pending_trx_callback ) _pending_trx_callback( fc::variant(trx) );
                      });

   // for data_transaction
   _data_transaction_change_connection = _db.data_transaction_changed_objects.connect([this](const string request_id) {
                                on_data_transaction_objects_changed(request_id);
                                });
}

database_api_impl::~database_api_impl()
{
}

fc::variants database_api_impl::get_objects(const vector<object_id_type>& ids)const
{
   if( _subscribe_callback )  {
      for( auto id : ids )
      {
         if( id.type() == operation_history_object_type && id.space() == protocol_ids ) continue;
         if( id.type() == impl_account_transaction_history_object_type && id.space() == implementation_ids ) continue;

         this->subscribe_to_item( id );
      }
   }

   fc::variants result;
   result.reserve(ids.size());

   std::transform(ids.begin(), ids.end(), std::back_inserter(result),
                  [this](object_id_type id) -> fc::variant {
      if(auto obj = _db.find_object(id))
         return obj->to_variant();
      return {};
   });

   return result;
}

fc::variants database_api_impl::get_table_objects(string code, string scope, string table) const
{
    fc::variants result;

    const auto &table_idx = _db.get_index_type<table_id_multi_index>().indices().get<by_code_scope_table>();
    auto existing_tid = table_idx.find(boost::make_tuple(name(code), name(code), name(table)));
    if (existing_tid != table_idx.end()) {
        const auto &kv_idx = _db.get_index_type<key_value_index>().indices().get<by_scope_primary>();
        decltype(existing_tid->id) next_tid(existing_tid->id + 1);
        auto lower = kv_idx.lower_bound(boost::make_tuple(existing_tid->id));
        auto upper = kv_idx.lower_bound(boost::make_tuple(next_tid));

        result.reserve(kv_idx.size());

        std::transform(lower, upper, std::back_inserter(result),
                       [](auto &kv) -> fc::variant {
                           return kv.to_variant();
                       });
    }
    return result;
}

void database_api_impl::set_subscribe_callback( std::function<void(const variant&)> cb, bool notify_remove_create )
{
   //edump((clear_filter));
   _subscribe_callback = cb;
   _notify_remove_create = notify_remove_create;
   _subscribed_accounts.clear();

   static fc::bloom_parameters param;
   param.projected_element_count    = 10000;
   param.false_positive_probability = 1.0/100;
   param.maximum_size = 1024*8*8*2;
   param.compute_optimal_parameters();
   _subscribe_filter = fc::bloom_filter(param);
}

void database_api_impl::set_data_transaction_subscribe_callback( std::function<void(const variant&)> cb, bool notify_remove_create )
{
   //edump((clear_filter));
   _data_transaction_subscribe_callback = cb;
}

void database_api_impl::set_data_transaction_products_subscribe_callback(std::function<void(const variant&)> cb, vector<object_id_type> ids)
{
    _data_transaction_subscribe_products = ids;
    _data_transaction_products_subscribe_callback = cb;
}

void database_api_impl::set_pending_transaction_callback( std::function<void(const variant&)> cb )
{
   _pending_trx_callback = cb;
}

void database_api_impl::set_block_applied_callback( std::function<void(const variant& block_id)> cb )
{
   _block_applied_callback = cb;
}

void database_api_impl::cancel_all_subscriptions()
{
   set_subscribe_callback( std::function<void(const fc::variant&)>(), true);
   _market_subscriptions.clear();
   _data_transaction_subscribe_callback =  std::function<void(const fc::variant&)>();
   _data_transaction_products_subscribe_callback =  std::function<void(const fc::variant&)>();
}

optional<block_header> database_api_impl::get_block_header(uint32_t block_num) const
{
   auto result = _db.fetch_block_by_number(block_num);
   if(result)
      return *result;
   return {};
}

optional<signed_block_with_info> database_api_impl::get_block(uint32_t block_num)const
{
   return _db.fetch_block_by_number(block_num);
}

optional<signed_block_with_info> database_api_impl::get_block_by_id(block_id_type block_id)const
{
   return _db.fetch_block_by_id(block_id);
}

processed_transaction database_api_impl::get_transaction(uint32_t block_num, uint32_t trx_num)const
{
   auto opt_block = _db.fetch_block_by_number(block_num);
   FC_ASSERT( opt_block );
   FC_ASSERT( opt_block->transactions.size() > trx_num );
   return opt_block->transactions[trx_num];
}

global_property_object database_api_impl::get_global_properties()const
{
   dlog("id=${id}................", ("id", global_property_id_type()));
   return _db.get(global_property_id_type());
}

data_transaction_commission_percent_t database_api_impl::get_commission_percent() const
{
    const chain_parameters& params = get_global_properties().parameters;
    for (auto& ext : params.extensions) {
        if (ext.which() == future_extensions::tag<data_transaction_commission_percent_t>::value) {
            return ext.get<data_transaction_commission_percent_t>();
        }
    }
    FC_THROW("no ommission_rate");
}

chain_property_object database_api_impl::get_chain_properties()const
{
   return _db.get(chain_property_id_type());
}

fc::variant_object database_api_impl::get_config()const
{
   return graphene::chain::get_config();
}

chain_id_type database_api_impl::get_chain_id()const
{
   return _db.get_chain_id();
}

dynamic_global_property_object database_api_impl::get_dynamic_global_properties()const
{
   return _db.get(dynamic_global_property_id_type());
}

/**
 *  @return all accounts that referr to the key or account id in their owner or active authorities.
 */
vector<vector<account_id_type>> database_api_impl::get_key_references( vector<public_key_type> keys )const
{
   wdump( (keys) );
   vector< vector<account_id_type> > final_result;
   final_result.reserve(keys.size());

   for( auto& key : keys )
   {

      address a1( pts_address(key, false, 56) );
      address a2( pts_address(key, true, 56) );
      address a3( pts_address(key, false, 0)  );
      address a4( pts_address(key, true, 0)  );
      address a5( key );

      subscribe_to_item( key );
      subscribe_to_item( a1 );
      subscribe_to_item( a2 );
      subscribe_to_item( a3 );
      subscribe_to_item( a4 );
      subscribe_to_item( a5 );

      const auto& idx = _db.get_index_type<account_index>();
      const auto& aidx = dynamic_cast<const primary_index<account_index>&>(idx);
      const auto& refs = aidx.get_secondary_index<graphene::chain::account_member_index>();
      auto itr = refs.account_to_key_memberships.find(key);
      vector<account_id_type> result;

      for( auto& a : {a1,a2,a3,a4,a5} )
      {
          auto itr = refs.account_to_address_memberships.find(a);
          if( itr != refs.account_to_address_memberships.end() )
          {
             result.reserve( itr->second.size() );
             for( auto item : itr->second )
             {
                wdump((a)(item)(item(_db).name));
                result.push_back(item);
             }
          }
      }

      if( itr != refs.account_to_key_memberships.end() )
      {
         result.reserve( itr->second.size() );
         for( auto item : itr->second ) result.push_back(item);
      }
      final_result.emplace_back( std::move(result) );
   }

   for( auto i : final_result )
      subscribe_to_item(i);

   return final_result;
}

bool database_api_impl::is_public_key_registered(string public_key) const
{
    // Short-circuit
    if (public_key.empty()) {
        return false;
    }

    // Search among all keys using an existing map of *current* account keys
    public_key_type key;
    try {
        key = public_key_type(public_key);
    } catch ( ... ) {
        // An invalid public key was detected
        return false;
    }
    const auto& idx = _db.get_index_type<account_index>();
    const auto& aidx = dynamic_cast<const primary_index<account_index>&>(idx);
    const auto& refs = aidx.get_secondary_index<graphene::chain::account_member_index>();
    auto itr = refs.account_to_key_memberships.find(key);
    bool is_known = itr != refs.account_to_key_memberships.end();

    return is_known;
}

bool database_api_impl::is_account_registered(string name) const
{
    if (name.empty()) {
        return false;
    }

    if (!is_valid_name(name)) {
        return false;
    }

    const auto& idx = _db.get_index_type<account_index>().indices().get<by_name>();
    auto iter = idx.find(name);
    bool is_known = iter != idx.end();
    return is_known;
}

vector<optional<account_object>> database_api_impl::get_accounts(const vector<account_id_type>& account_ids)const
{
   vector<optional<account_object>> result; result.reserve(account_ids.size());
   std::transform(account_ids.begin(), account_ids.end(), std::back_inserter(result),
                  [this](account_id_type id) -> optional<account_object> {
      if(auto o = _db.find(id))
      {
         subscribe_to_item( id );
         return *o;
      }
      return {};
   });
   return result;
}

std::map<std::string, full_account> database_api_impl::get_full_accounts( const vector<std::string>& names_or_ids, bool subscribe)
{
   dlog("names_or_ids ${n}", ("n", names_or_ids));
   std::map<std::string, full_account> results;

   for (const std::string& account_name_or_id : names_or_ids)
   {
      const account_object* account = nullptr;
      if (std::isdigit(account_name_or_id[0]))
         account = _db.find(fc::variant(account_name_or_id).as<account_id_type>());
      else
      {
         const auto& idx = _db.get_index_type<account_index>().indices().get<by_name>();
         auto itr = idx.find(account_name_or_id);
         if (itr != idx.end())
            account = &*itr;
      }
      if (account == nullptr)
         continue;

      if( subscribe )
      {
         FC_ASSERT(_subscribed_accounts.size() <= 100);
         _subscribed_accounts.insert( account->get_id() );
         subscribe_to_item( account->id );
      }

      // fc::mutable_variant_object full_account;
      full_account acnt;
      acnt.account = *account;
      acnt.statistics = account->statistics(_db);
      acnt.registrar_name = account->registrar(_db).name;
      acnt.referrer_name = account->referrer(_db).name;
      acnt.lifetime_referrer_name = account->lifetime_referrer(_db).name;
      acnt.votes = lookup_vote_ids( vector<vote_id_type>(account->options.votes.begin(),account->options.votes.end()) );

      // Add the account itself, its statistics object, cashback balance, and referral account names
      /*
      full_account("account", *account)("statistics", account->statistics(_db))
            ("registrar_name", account->registrar(_db).name)("referrer_name", account->referrer(_db).name)
            ("lifetime_referrer_name", account->lifetime_referrer(_db).name);
            */
      if (account->cashback_vb)
      {
         acnt.cashback_balance = account->cashback_balance(_db);
      }
      // Add the account's proposals
      const auto& proposal_idx = _db.get_index_type<proposal_index>();
      const auto& pidx = dynamic_cast<const primary_index<proposal_index>&>(proposal_idx);
      const auto& proposals_by_account = pidx.get_secondary_index<graphene::chain::required_approval_index>();
      auto  required_approvals_itr = proposals_by_account._account_to_proposals.find( account->id );
      if( required_approvals_itr != proposals_by_account._account_to_proposals.end() )
      {
         acnt.proposals.reserve( required_approvals_itr->second.size() );
         for( auto proposal_id : required_approvals_itr->second )
            acnt.proposals.push_back( proposal_id(_db) );
      }


      // Add the account's balances
      auto balance_range = _db.get_index_type<account_balance_index>().indices().get<by_account_asset>().equal_range(boost::make_tuple(account->id));
      //vector<account_balance_object> balances;
      std::for_each(balance_range.first, balance_range.second,
                    [&acnt](const account_balance_object& balance) {
                       acnt.balances.emplace_back(balance);
                    });
    
      auto balance_locked_range = _db.get_index_type<account_balance_locked_index>().indices().get<by_account_asset>().equal_range(boost::make_tuple(account->id));
      //vector<account_balance_locked_object> locked_balances;
      std::for_each(balance_locked_range.first, balance_locked_range.second,
                    [&acnt](const lock_balance_object& lock_balance) {
                       acnt.locked_balances.emplace_back(lock_balance);
                    });

      // Add the account's vesting balances
      auto vesting_range = _db.get_index_type<vesting_balance_index>().indices().get<by_account>().equal_range(account->id);
      std::for_each(vesting_range.first, vesting_range.second,
                    [&acnt](const vesting_balance_object& balance) {
                       acnt.vesting_balances.emplace_back(balance);
                    });

      // Add the account's orders
      auto order_range = _db.get_index_type<limit_order_index>().indices().get<by_account>().equal_range(account->id);
      std::for_each(order_range.first, order_range.second,
                    [&acnt] (const limit_order_object& order) {
                       acnt.limit_orders.emplace_back(order);
                    });
      auto call_range = _db.get_index_type<call_order_index>().indices().get<by_account>().equal_range(account->id);
      std::for_each(call_range.first, call_range.second,
                    [&acnt] (const call_order_object& call) {
                       acnt.call_orders.emplace_back(call);
                    });

      // get assets issued by user
      auto asset_range = _db.get_index_type<asset_index>().indices().get<by_issuer>().equal_range(account->id);
      std::for_each(asset_range.first, asset_range.second,
                    [&acnt] (const asset_object& asset) {
                       acnt.assets.emplace_back(asset.id);
                    });

      // get withdraws permissions
      auto withdraw_range = _db.get_index_type<withdraw_permission_index>().indices().get<by_from>().equal_range(account->id);
      std::for_each(withdraw_range.first, withdraw_range.second,
                    [&acnt] (const withdraw_permission_object& withdraw) {
                       acnt.withdraws.emplace_back(withdraw);
                    });


      results[account_name_or_id] = acnt;
   }
   return results;
}

optional<account_object> database_api_impl::get_account_by_name( string name )const
{
   const auto& idx = _db.get_index_type<account_index>().indices().get<by_name>();
   auto itr = idx.find(name);
   if (itr != idx.end())
      return *itr;
   return optional<account_object>();
}

vector<account_id_type> database_api_impl::get_account_references( account_id_type account_id )const
{
   const auto& idx = _db.get_index_type<account_index>();
   const auto& aidx = dynamic_cast<const primary_index<account_index>&>(idx);
   const auto& refs = aidx.get_secondary_index<graphene::chain::account_member_index>();
   auto itr = refs.account_to_account_memberships.find(account_id);
   vector<account_id_type> result;

   if( itr != refs.account_to_account_memberships.end() )
   {
      result.reserve( itr->second.size() );
      for( auto item : itr->second ) result.push_back(item);
   }
   return result;
}

vector<optional<account_object>> database_api_impl::lookup_account_names(const vector<string>& account_names)const
{
   const auto& accounts_by_name = _db.get_index_type<account_index>().indices().get<by_name>();
   vector<optional<account_object> > result;
   result.reserve(account_names.size());
   std::transform(account_names.begin(), account_names.end(), std::back_inserter(result),
                  [&accounts_by_name](const string& name) -> optional<account_object> {
      auto itr = accounts_by_name.find(name);
      return itr == accounts_by_name.end()? optional<account_object>() : *itr;
   });
   return result;
}

map<string,account_id_type> database_api_impl::lookup_accounts(const string& lower_bound_name, uint32_t limit)const
{
   FC_ASSERT( limit <= 1000 );
   const auto& accounts_by_name = _db.get_index_type<account_index>().indices().get<by_name>();
   map<string,account_id_type> result;

   for( auto itr = accounts_by_name.lower_bound(lower_bound_name);
        limit-- && itr != accounts_by_name.end();
        ++itr )
   {
      result.insert(make_pair(itr->name, itr->get_id()));
      if( limit == 1 )
         subscribe_to_item( itr->get_id() );
   }

   return result;
}

uint64_t database_api_impl::get_account_count()const
{
   return _db.get_index_type<account_index>().indices().size();
}

vector<asset> database_api_impl::get_account_balances(account_id_type acnt, const flat_set<asset_id_type>& assets)const
{
   vector<asset> result;
   if (assets.empty())
   {
      // if the caller passes in an empty list of assets, return balances for all assets the account owns
      const account_balance_index& balance_index = _db.get_index_type<account_balance_index>();
      auto range = balance_index.indices().get<by_account_asset>().equal_range(boost::make_tuple(acnt));
      for (const account_balance_object& balance : boost::make_iterator_range(range.first, range.second))
         result.push_back(asset(balance.get_balance()));
   }
   else
   {
      result.reserve(assets.size());

      std::transform(assets.begin(), assets.end(), std::back_inserter(result),
                     [this, acnt](asset_id_type id) { return _db.get_balance(acnt, id); });
   }

   return result;
}

vector<asset> database_api_impl::get_account_lock_balances(account_id_type acnt, const flat_set<asset_id_type>& assets)const
{
    vector<asset> result;
    flat_set<asset_id_type> asset_ids;
    const account_balance_locked_index& lock_balance_index = _db.get_index_type<account_balance_locked_index>();
    auto lock_blance_range = lock_balance_index.indices().get<by_account_asset>().equal_range(boost::make_tuple(acnt));

    if (assets.empty())
    {
       // if the caller passes in an empty list of assets, return lock balances for all assets the account 
       const account_balance_index& balance_index = _db.get_index_type<account_balance_index>();
       auto balance_range = balance_index.indices().get<by_account_asset>().equal_range(boost::make_tuple(acnt));

       for (const account_balance_object& balance : boost::make_iterator_range(balance_range.first, balance_range.second))
       {
          asset_ids.insert(balance.asset_type);
       }
    }
    else
    {
       asset_ids = assets;
    }

    result.reserve(assets.size());
    for (const asset_id_type& asset_id : asset_ids)
    {
       share_type amount = 0;
       for (const lock_balance_object& lock_balance : boost::make_iterator_range(lock_blance_range.first, lock_blance_range.second))
       {
          if (asset_id == lock_balance.amount.asset_id)
          {
             amount += lock_balance.amount.amount;
          }
       }
       result.push_back({amount, asset_id});
    }

    return result;
}

vector<asset> database_api_impl::get_named_account_balances(const std::string& name, const flat_set<asset_id_type>& assets) const
{
   const auto& accounts_by_name = _db.get_index_type<account_index>().indices().get<by_name>();
   auto itr = accounts_by_name.find(name);
   FC_ASSERT( itr != accounts_by_name.end() );
   return get_account_balances(itr->get_id(), assets);
}

vector<balance_object> database_api_impl::get_balance_objects( const vector<address>& addrs )const
{
   try
   {
      const auto& bal_idx = _db.get_index_type<balance_index>();
      const auto& by_owner_idx = bal_idx.indices().get<by_owner>();

      vector<balance_object> result;

      for( const auto& owner : addrs )
      {
         subscribe_to_item( owner );
         auto itr = by_owner_idx.lower_bound( boost::make_tuple( owner, asset_id_type(0) ) );
         while( itr != by_owner_idx.end() && itr->owner == owner )
         {
            result.push_back( *itr );
            ++itr;
         }
      }
      return result;
   }
   FC_CAPTURE_AND_RETHROW( (addrs) )
}

vector<asset> database_api_impl::get_vested_balances( const vector<balance_id_type>& objs )const
{
   try
   {
      vector<asset> result;
      result.reserve( objs.size() );
      auto now = _db.head_block_time();
      for( auto obj : objs )
         result.push_back( obj(_db).available( now ) );
      return result;
   } FC_CAPTURE_AND_RETHROW( (objs) )
}

vector<vesting_balance_object> database_api_impl::get_vesting_balances( account_id_type account_id )const
{
   try
   {
      vector<vesting_balance_object> result;
      auto vesting_range = _db.get_index_type<vesting_balance_index>().indices().get<by_account>().equal_range(account_id);
      std::for_each(vesting_range.first, vesting_range.second,
                    [&result](const vesting_balance_object& balance) {
                       result.emplace_back(balance);
                    });
      return result;
   }
   FC_CAPTURE_AND_RETHROW( (account_id) );
}

vector<optional<asset_object>> database_api_impl::get_assets(const vector<asset_id_type>& asset_ids)const
{
   vector<optional<asset_object>> result; result.reserve(asset_ids.size());
   std::transform(asset_ids.begin(), asset_ids.end(), std::back_inserter(result),
                  [this](asset_id_type id) -> optional<asset_object> {
      if(auto o = _db.find(id))
      {
         subscribe_to_item( id );
         return *o;
      }
      return {};
   });
   return result;
}

vector<asset_object> database_api_impl::list_assets(const string& lower_bound_symbol, uint32_t limit)const
{
   FC_ASSERT( limit <= 100 );
   const auto& assets_by_symbol = _db.get_index_type<asset_index>().indices().get<by_symbol>();
   vector<asset_object> result;
   result.reserve(limit);

   auto itr = assets_by_symbol.lower_bound(lower_bound_symbol);

   if( lower_bound_symbol == "" )
      itr = assets_by_symbol.begin();

   while(limit-- && itr != assets_by_symbol.end())
      result.emplace_back(*itr++);

   return result;
}

vector<optional<asset_object>> database_api_impl::lookup_asset_symbols(const vector<string>& symbols_or_ids)const
{
   const auto& assets_by_symbol = _db.get_index_type<asset_index>().indices().get<by_symbol>();
   vector<optional<asset_object> > result;
   result.reserve(symbols_or_ids.size());
   std::transform(symbols_or_ids.begin(), symbols_or_ids.end(), std::back_inserter(result),
                  [this, &assets_by_symbol](const string& symbol_or_id) -> optional<asset_object> {
      if( !symbol_or_id.empty() && std::isdigit(symbol_or_id[0]) )
      {
         auto ptr = _db.find(variant(symbol_or_id).as<asset_id_type>());
         return ptr == nullptr? optional<asset_object>() : *ptr;
      }
      auto itr = assets_by_symbol.find(symbol_or_id);
      return itr == assets_by_symbol.end()? optional<asset_object>() : *itr;
   });
   return result;
}

uint64_t database_api_impl::get_data_transaction_product_costs(fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_total_count(fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_merchants_total_count() const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_commission(fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_pay_fee(fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}


uint64_t database_api_impl::get_data_transaction_product_costs_by_requester(string requester, fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_total_count_by_requester(string requester, fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_pay_fees_by_requester(string requester, fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_product_costs_by_product_id(string product_id, fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

uint64_t database_api_impl::get_data_transaction_total_count_by_product_id(string product_id, fc::time_point_sec start, fc::time_point_sec end) const
{
    return 0;
}

map<account_id_type, uint64_t> database_api_impl::list_data_transaction_complain_requesters(fc::time_point_sec start_date_time, fc::time_point_sec end_date_time, uint8_t limit) const
{
    const auto& idx = _db.get_index_type<data_transaction_complain_index>().indices().get<by_create_date_time>();
    auto itr_start = idx.lower_bound(start_date_time);
    auto itr_end = idx.upper_bound(end_date_time);

    map<account_id_type, uint64_t> accounts;
    for(const auto& obj : boost::make_iterator_range(itr_start, itr_end)){
        accounts[obj.requester] = 0;
    }
    for(const auto& obj : boost::make_iterator_range(itr_start, itr_end)){
        ++accounts[obj.requester];
    }

    //sort
    vector<PAIR> requesters(accounts.begin(), accounts.end());
    sort(requesters.begin(), requesters.end(), cmp_pair_by_value());

    std::map<account_id_type, uint64_t> results;
    for(uint64_t i = 0; i < limit && i < requesters.size(); ++i){
        results.insert(requesters.at(i));
    }

    return results;
}

map<account_id_type, uint64_t> database_api_impl::list_data_transaction_complain_datasources(fc::time_point_sec start_date_time, fc::time_point_sec end_date_time, uint8_t limit) const
{
    const auto& idx = _db.get_index_type<data_transaction_complain_index>().indices().get<by_create_date_time>();
    auto itr_start = idx.lower_bound(start_date_time);
    auto itr_end = idx.upper_bound(end_date_time);

    map<account_id_type, uint64_t> accounts;
    for (const auto& obj : boost::make_iterator_range(itr_start, itr_end)) {
        accounts[obj.datasource] = 0;
    }
    for (const auto& obj : boost::make_iterator_range(itr_start, itr_end)) {
        ++accounts[obj.datasource];
    }

    // sort
    vector<PAIR> datasources(accounts.begin(), accounts.end());
    sort(datasources.begin(), datasources.end(), cmp_pair_by_value());

    std::map<account_id_type, uint64_t> results;
    for (uint8_t i = 0; i < limit && i < datasources.size(); ++i) {
        results.insert(datasources.at(i));
    }

    return results;
}

/**
 *  @return the limit orders for both sides of the book for the two assets specified up to limit number on each side.
 */
vector<limit_order_object> database_api_impl::get_limit_orders(asset_id_type a, asset_id_type b, uint32_t limit)const
{
   const auto& limit_order_idx = _db.get_index_type<limit_order_index>();
   const auto& limit_price_idx = limit_order_idx.indices().get<by_price>();

   vector<limit_order_object> result;

   uint32_t count = 0;
   auto limit_itr = limit_price_idx.lower_bound(price::max(a,b));
   auto limit_end = limit_price_idx.upper_bound(price::min(a,b));
   while(limit_itr != limit_end && count < limit)
   {
      result.push_back(*limit_itr);
      ++limit_itr;
      ++count;
   }
   count = 0;
   limit_itr = limit_price_idx.lower_bound(price::max(b,a));
   limit_end = limit_price_idx.upper_bound(price::min(b,a));
   while(limit_itr != limit_end && count < limit)
   {
      result.push_back(*limit_itr);
      ++limit_itr;
      ++count;
   }

   return result;
}

vector<call_order_object> database_api_impl::get_call_orders(asset_id_type a, uint32_t limit)const
{
   const auto& call_index = _db.get_index_type<call_order_index>().indices().get<by_price>();
   const asset_object& mia = _db.get(a);
   price index_price = price::min(mia.bitasset_data(_db).options.short_backing_asset, mia.get_id());

   return vector<call_order_object>(call_index.lower_bound(index_price.min()),
                                    call_index.lower_bound(index_price.max()));
}

vector<force_settlement_object> database_api_impl::get_settle_orders(asset_id_type a, uint32_t limit)const
{
   const auto& settle_index = _db.get_index_type<force_settlement_index>().indices().get<by_expiration>();
   const asset_object& mia = _db.get(a);
   return vector<force_settlement_object>(settle_index.lower_bound(mia.get_id()),
                                          settle_index.upper_bound(mia.get_id()));
}

vector<call_order_object> database_api_impl::get_margin_positions( const account_id_type& id )const
{
   try
   {
      const auto& idx = _db.get_index_type<call_order_index>();
      const auto& aidx = idx.indices().get<by_account>();
      auto start = aidx.lower_bound( boost::make_tuple( id, asset_id_type(0) ) );
      auto end = aidx.lower_bound( boost::make_tuple( id+1, asset_id_type(0) ) );
      vector<call_order_object> result;
      while( start != end )
      {
         result.push_back(*start);
         ++start;
      }
      return result;
   } FC_CAPTURE_AND_RETHROW( (id) )
}

void database_api_impl::subscribe_to_market(std::function<void(const variant&)> callback, asset_id_type a, asset_id_type b)
{
   if(a > b) std::swap(a,b);
   FC_ASSERT(a != b);
   _market_subscriptions[ std::make_pair(a,b) ] = callback;
}

void database_api_impl::unsubscribe_data_transaction_callback()
{
    dlog("unsubscribe_data_transaction_callback");
    _data_transaction_subscribe_callback =  std::function<void(const fc::variant&)>();
}

void database_api_impl::unsubscribe_from_market(asset_id_type a, asset_id_type b)
{
   if(a > b) std::swap(a,b);
   FC_ASSERT(a != b);
   _market_subscriptions.erase(std::make_pair(a,b));
}

market_ticker database_api_impl::get_ticker( const string& base, const string& quote )const
{
   auto assets = lookup_asset_symbols( {base, quote} );
   FC_ASSERT( assets[0], "Invalid base asset symbol: ${s}", ("s",base) );
   FC_ASSERT( assets[1], "Invalid quote asset symbol: ${s}", ("s",quote) );

   auto base_id = assets[0]->id;
   auto quote_id = assets[1]->id;

   market_ticker result;

   result.base = base;
   result.quote = quote;
   result.base_volume = 0;
   result.quote_volume = 0;
   result.percent_change = 0;
   result.lowest_ask = 0;
   result.highest_bid = 0;

   try {
      if( base_id > quote_id ) std::swap(base_id, quote_id);

      uint32_t day = 86400;
      auto now = fc::time_point_sec( fc::time_point::now() );
      auto trades = get_trade_history( base, quote, now, fc::time_point_sec( now.sec_since_epoch() - day ), 100 );

      if ( trades.size() )
      {
         result.latest = trades[0].price;

         for ( market_trade t: trades )
         {
            result.base_volume += t.value;
            result.quote_volume += t.amount;
         }

         while (trades.size() == 100)
         {
            trades = get_trade_history( base, quote, trades[99].date, fc::time_point_sec( now.sec_since_epoch() - day ), 100 );

            for ( market_trade t: trades )
            {
               result.base_volume += t.value;
               result.quote_volume += t.amount;
            }
         }

         trades = get_trade_history( base, quote, trades.back().date, fc::time_point_sec(), 1 );
         result.percent_change = trades.size() > 0 ? ( ( result.latest / trades.back().price ) - 1 ) * 100 : 0;
      }

      auto orders = get_order_book( base, quote, 1 );
      if( orders.asks.size() )
         result.lowest_ask = orders.asks[0].price;
      if( orders.bids.size() )
         result.highest_bid = orders.bids[0].price;

   } FC_CAPTURE_AND_RETHROW( (base)(quote) )

   return result;
}

market_volume database_api_impl::get_24_volume( const string& base, const string& quote )const
{
   auto assets = lookup_asset_symbols( {base, quote} );
   FC_ASSERT( assets[0], "Invalid base asset symbol: ${s}", ("s",base) );
   FC_ASSERT( assets[1], "Invalid quote asset symbol: ${s}", ("s",quote) );

   auto base_id = assets[0]->id;
   auto quote_id = assets[1]->id;

   market_volume result;
   result.base = base;
   result.quote = quote;
   result.base_volume = 0;
   result.quote_volume = 0;

   try {
      if( base_id > quote_id ) std::swap(base_id, quote_id);

      uint32_t bucket_size = 86400;
      auto now = fc::time_point_sec( fc::time_point::now() );

      auto trades = get_trade_history( base, quote, now, fc::time_point_sec( now.sec_since_epoch() - bucket_size ), 100 );

      for ( market_trade t: trades )
      {
         result.base_volume += t.value;
         result.quote_volume += t.amount;
      }

      while (trades.size() == 100)
      {
         trades = get_trade_history( base, quote, trades[99].date, fc::time_point_sec( now.sec_since_epoch() - bucket_size ), 100 );

         for ( market_trade t: trades )
         {
            result.base_volume += t.value;
            result.quote_volume += t.amount;
         }
      }

      return result;
   } FC_CAPTURE_AND_RETHROW( (base)(quote) )
}

order_book database_api_impl::get_order_book( const string& base, const string& quote, unsigned limit )const
{
   using boost::multiprecision::uint128_t;
   FC_ASSERT( limit <= 50 );

   order_book result;
   result.base = base;
   result.quote = quote;

   auto assets = lookup_asset_symbols( {base, quote} );
   FC_ASSERT( assets[0], "Invalid base asset symbol: ${s}", ("s",base) );
   FC_ASSERT( assets[1], "Invalid quote asset symbol: ${s}", ("s",quote) );

   auto base_id = assets[0]->id;
   auto quote_id = assets[1]->id;
   auto orders = get_limit_orders( base_id, quote_id, limit );


   auto asset_to_real = [&]( const asset& a, int p ) { return double(a.amount.value)/pow( 10, p ); };
   auto price_to_real = [&]( const price& p )
   {
      if( p.base.asset_id == base_id )
         return asset_to_real( p.base, assets[0]->precision ) / asset_to_real( p.quote, assets[1]->precision );
      else
         return asset_to_real( p.quote, assets[0]->precision ) / asset_to_real( p.base, assets[1]->precision );
   };

   for( const auto& o : orders )
   {
      if( o.sell_price.base.asset_id == base_id )
      {
         order ord;
         ord.price = price_to_real( o.sell_price );
         ord.quote = asset_to_real( share_type( ( uint128_t( o.for_sale.value ) * o.sell_price.quote.amount.value ) / o.sell_price.base.amount.value ), assets[1]->precision );
         ord.base = asset_to_real( o.for_sale, assets[0]->precision );
         result.bids.push_back( ord );
      }
      else
      {
         order ord;
         ord.price = price_to_real( o.sell_price );
         ord.quote = asset_to_real( o.for_sale, assets[1]->precision );
         ord.base = asset_to_real( share_type( ( uint64_t( o.for_sale.value ) * o.sell_price.quote.amount.value ) / o.sell_price.base.amount.value ), assets[0]->precision );
         result.asks.push_back( ord );
      }
   }

   return result;
}

optional<pocs_object> database_api_impl::get_pocs_object(league_id_type league_id, account_id_type account_id, object_id_type product_id    )const
{
    FC_ASSERT(get_leagues({league_id}).size());
    FC_ASSERT(get_objects({product_id}).size());
    FC_ASSERT(get_accounts({account_id}).size());

    optional<pocs_object> result;
    const auto& idx = _db.get_index_type<pocs_index>().indices().get<by_multi_id>();
    auto itr = idx.find(boost::make_tuple(league_id, account_id, product_id));
    if (itr != idx.end()) {
        return *itr;
    }

    return optional<pocs_object>();
}

vector<market_trade> database_api_impl::get_trade_history( const string& base,
                                                           const string& quote,
                                                           fc::time_point_sec start,
                                                           fc::time_point_sec stop,
                                                           unsigned limit )const
{
   FC_ASSERT( limit <= 100 );

   auto assets = lookup_asset_symbols( {base, quote} );
   FC_ASSERT( assets[0], "Invalid base asset symbol: ${s}", ("s",base) );
   FC_ASSERT( assets[1], "Invalid quote asset symbol: ${s}", ("s",quote) );

   auto base_id = assets[0]->id;
   auto quote_id = assets[1]->id;

   if( base_id > quote_id ) std::swap( base_id, quote_id );
   const auto& history_idx = _db.get_index_type<graphene::market_history::history_index>().indices().get<by_key>();
   history_key hkey;
   hkey.base = base_id;
   hkey.quote = quote_id;
   hkey.sequence = std::numeric_limits<int64_t>::min();

   auto price_to_real = [&]( const share_type a, int p ) { return double( a.value ) / pow( 10, p ); };

   if ( start.sec_since_epoch() == 0 )
      start = fc::time_point_sec( fc::time_point::now() );

   uint32_t count = 0;
   auto itr = history_idx.lower_bound( hkey );
   vector<market_trade> result;

   while( itr != history_idx.end() && count < limit && !( itr->key.base != base_id || itr->key.quote != quote_id || itr->time < stop ) )
   {
      if( itr->time < start )
      {
         market_trade trade;

         if( assets[0]->id == itr->op.receives.asset_id )
         {
            trade.amount = price_to_real( itr->op.pays.amount, assets[1]->precision );
            trade.value = price_to_real( itr->op.receives.amount, assets[0]->precision );
         }
         else
         {
            trade.amount = price_to_real( itr->op.receives.amount, assets[1]->precision );
            trade.value = price_to_real( itr->op.pays.amount, assets[0]->precision );
         }

         trade.date = itr->time;
         trade.price = trade.value / trade.amount;

         result.push_back( trade );
         ++count;
      }

      // Trades are tracked in each direction.
      ++itr;
      ++itr;
   }

   return result;
}

vector<optional<witness_object>> database_api_impl::get_witnesses(const vector<witness_id_type>& witness_ids)const
{
   vector<optional<witness_object>> result; result.reserve(witness_ids.size());
   std::transform(witness_ids.begin(), witness_ids.end(), std::back_inserter(result),
                  [this](witness_id_type id) -> optional<witness_object> {
      if(auto o = _db.find(id))
         return *o;
      return {};
   });
   return result;
}

fc::optional<witness_object> database_api_impl::get_witness_by_account(account_id_type account) const
{
   const auto& idx = _db.get_index_type<witness_index>().indices().get<by_account>();
   auto itr = idx.find(account);
   if( itr != idx.end() )
      return *itr;
   return {};
}

map<string, witness_id_type> database_api_impl::lookup_witness_accounts(const string& lower_bound_name, uint32_t limit)const
{
   FC_ASSERT( limit <= 1000 );
   const auto& witnesses_by_id = _db.get_index_type<witness_index>().indices().get<by_id>();

   // we want to order witnesses by account name, but that name is in the account object
   // so the witness_index doesn't have a quick way to access it.
   // get all the names and look them all up, sort them, then figure out what
   // records to return.  This could be optimized, but we expect the
   // number of witnesses to be few and the frequency of calls to be rare
   std::map<std::string, witness_id_type> witnesses_by_account_name;
   for (const witness_object& witness : witnesses_by_id)
       if (auto account_iter = _db.find(witness.witness_account))
           if (account_iter->name >= lower_bound_name) // we can ignore anything below lower_bound_name
               witnesses_by_account_name.insert(std::make_pair(account_iter->name, witness.id));

   auto end_iter = witnesses_by_account_name.begin();
   while (end_iter != witnesses_by_account_name.end() && limit--)
       ++end_iter;
   witnesses_by_account_name.erase(end_iter, witnesses_by_account_name.end());
   return witnesses_by_account_name;
}

uint64_t database_api_impl::get_witness_count()const
{
   return _db.get_index_type<witness_index>().indices().size();
}

vector<optional<committee_member_object>> database_api_impl::get_committee_members(const vector<committee_member_id_type>& committee_member_ids)const
{
   vector<optional<committee_member_object>> result; result.reserve(committee_member_ids.size());
   std::transform(committee_member_ids.begin(), committee_member_ids.end(), std::back_inserter(result),
                  [this](committee_member_id_type id) -> optional<committee_member_object> {
      if(auto o = _db.find(id))
         return *o;
      return {};
   });
   return result;
}

fc::optional<committee_member_object> database_api_impl::get_committee_member_by_account(account_id_type account) const
{
   const auto& idx = _db.get_index_type<committee_member_index>().indices().get<by_account>();
   auto itr = idx.find(account);
   if( itr != idx.end() )
      return *itr;
   return {};
}

map<string, committee_member_id_type> database_api_impl::lookup_committee_member_accounts(const string& lower_bound_name, uint32_t limit)const
{
   FC_ASSERT( limit <= 1000 );
   const auto& committee_members_by_id = _db.get_index_type<committee_member_index>().indices().get<by_id>();

   // we want to order committee_members by account name, but that name is in the account object
   // so the committee_member_index doesn't have a quick way to access it.
   // get all the names and look them all up, sort them, then figure out what
   // records to return.  This could be optimized, but we expect the
   // number of committee_members to be few and the frequency of calls to be rare
   std::map<std::string, committee_member_id_type> committee_members_by_account_name;
   for (const committee_member_object& committee_member : committee_members_by_id)
       if (auto account_iter = _db.find(committee_member.committee_member_account))
           if (account_iter->name >= lower_bound_name) // we can ignore anything below lower_bound_name
               committee_members_by_account_name.insert(std::make_pair(account_iter->name, committee_member.id));

   auto end_iter = committee_members_by_account_name.begin();
   while (end_iter != committee_members_by_account_name.end() && limit--)
       ++end_iter;
   committee_members_by_account_name.erase(end_iter, committee_members_by_account_name.end());
   return committee_members_by_account_name;
}

vector<variant> database_api_impl::lookup_vote_ids( const vector<vote_id_type>& votes )const
{
   FC_ASSERT( votes.size() < 1000, "Only 1000 votes can be queried at a time" );

   const auto& witness_idx = _db.get_index_type<witness_index>().indices().get<by_vote_id>();
   const auto& committee_idx = _db.get_index_type<committee_member_index>().indices().get<by_vote_id>();
   const auto& for_worker_idx = _db.get_index_type<worker_index>().indices().get<by_vote_for>();
   const auto& against_worker_idx = _db.get_index_type<worker_index>().indices().get<by_vote_against>();

   vector<variant> result;
   result.reserve( votes.size() );
   for( auto id : votes )
   {
      switch( id.type() )
      {
         case vote_id_type::committee:
         {
            auto itr = committee_idx.find( id );
            if( itr != committee_idx.end() )
               result.emplace_back( variant( *itr ) );
            else
               result.emplace_back( variant() );
            break;
         }
         case vote_id_type::witness:
         {
            auto itr = witness_idx.find( id );
            if( itr != witness_idx.end() )
               result.emplace_back( variant( *itr ) );
            else
               result.emplace_back( variant() );
            break;
         }
         case vote_id_type::worker:
         {
            auto itr = for_worker_idx.find( id );
            if( itr != for_worker_idx.end() ) {
               result.emplace_back( variant( *itr ) );
            }
            else {
               auto itr = against_worker_idx.find( id );
               if( itr != against_worker_idx.end() ) {
                  result.emplace_back( variant( *itr ) );
               }
               else {
                  result.emplace_back( variant() );
               }
            }
            break;
         }
         case vote_id_type::VOTE_TYPE_COUNT: break; // supress unused enum value warnings
      }
   }
   return result;
}

std::string database_api_impl::get_transaction_hex(const signed_transaction& trx)const
{
   return fc::to_hex(fc::raw::pack(trx));
}

set<public_key_type> database_api_impl::get_required_signatures( const signed_transaction& trx, const flat_set<public_key_type>& available_keys )const
{
   dlog("trx ${trx}, available_keys ${available_keys}", ("trx", trx)("available_keys", available_keys));
   auto result = trx.get_required_signatures( _db.get_chain_id(),
                                       available_keys,
                                       [&]( account_id_type id ){ return &id(_db).active; },
                                       [&]( account_id_type id ){ return &id(_db).owner; },
                                       _db.get_global_properties().parameters.max_authority_depth );
   dlog("result ${result}",("result", result));
   return result;
}

set<public_key_type> database_api_impl::get_potential_signatures( const signed_transaction& trx )const
{
   dlog("trx ${trx}", ("trx", trx));
   set<public_key_type> result;
   trx.get_required_signatures(
      _db.get_chain_id(),
      flat_set<public_key_type>(),
      [&]( account_id_type id )
      {
         const auto& auth = id(_db).active;
         for( const auto& k : auth.get_keys() )
            result.insert(k);
         return &auth;
      },
      [&]( account_id_type id )
      {
         const auto& auth = id(_db).owner;
         for( const auto& k : auth.get_keys() )
            result.insert(k);
         return &auth;
      },
      _db.get_global_properties().parameters.max_authority_depth
   );

   dlog("result ${result}",("result", result));
   return result;
}

set<address> database_api_impl::get_potential_address_signatures( const signed_transaction& trx )const
{
   set<address> result;
   trx.get_required_signatures(
      _db.get_chain_id(),
      flat_set<public_key_type>(),
      [&]( account_id_type id )
      {
         const auto& auth = id(_db).active;
         for( const auto& k : auth.get_addresses() )
            result.insert(k);
         return &auth;
      },
      [&]( account_id_type id )
      {
         const auto& auth = id(_db).owner;
         for( const auto& k : auth.get_addresses() )
            result.insert(k);
         return &auth;
      },
      _db.get_global_properties().parameters.max_authority_depth
   );
   return result;
}

bool database_api_impl::verify_authority( const signed_transaction& trx )const
{
   trx.verify_authority( _db.get_chain_id(),
                         [&]( account_id_type id ){ return &id(_db).active; },
                         [&]( account_id_type id ){ return &id(_db).owner; },
                          _db.get_global_properties().parameters.max_authority_depth );
   return true;
}

bool database_api_impl::verify_account_authority( const string& name_or_id, const flat_set<public_key_type>& keys )const
{
   FC_ASSERT( name_or_id.size() > 0);
   const account_object* account = nullptr;
   if (std::isdigit(name_or_id[0]))
      account = _db.find(fc::variant(name_or_id).as<account_id_type>());
   else
   {
      const auto& idx = _db.get_index_type<account_index>().indices().get<by_name>();
      auto itr = idx.find(name_or_id);
      if (itr != idx.end())
         account = &*itr;
   }
   FC_ASSERT( account, "no such account" );


   /// reuse trx.verify_authority by creating a dummy transfer
   signed_transaction trx;
   transfer_operation op;
   op.from = account->id;
   trx.operations.emplace_back(op);

   return verify_authority( trx );
}

processed_transaction database_api_impl::validate_transaction( const signed_transaction& trx )const
{
   return _db.validate_transaction(trx);
}

/**
 * Container method for mutually recursive functions used to
 * implement get_required_fees() with potentially nested proposals.
 */
struct get_required_fees_helper
{
   get_required_fees_helper(
      const fee_schedule& _current_fee_schedule,
      const price& _core_exchange_rate,
      uint32_t _max_recursion
      )
      : current_fee_schedule(_current_fee_schedule),
        core_exchange_rate(_core_exchange_rate),
        max_recursion(_max_recursion)
   {}

   fc::variant set_op_fees( operation& op )
   {
      if( op.which() == operation::tag<proposal_create_operation>::value )
      {
         return set_proposal_create_op_fees( op );
      }
      else
      {
         asset fee = current_fee_schedule.set_fee( op, core_exchange_rate );
         fc::variant result;
         fc::to_variant( fee, result );
         return result;
      }
   }

   fc::variant set_proposal_create_op_fees( operation& proposal_create_op )
   {
      proposal_create_operation& op = proposal_create_op.get<proposal_create_operation>();
      std::pair< asset, fc::variants > result;
      for( op_wrapper& prop_op : op.proposed_ops )
      {
         FC_ASSERT( current_recursion < max_recursion );
         ++current_recursion;
         result.second.push_back( set_op_fees( prop_op.op ) );
         --current_recursion;
      }
      // we need to do this on the boxed version, which is why we use
      // two mutually recursive functions instead of a visitor
      result.first = current_fee_schedule.set_fee( proposal_create_op, core_exchange_rate );
      fc::variant vresult;
      fc::to_variant( result, vresult );
      return vresult;
   }

   const fee_schedule& current_fee_schedule;
   const price& core_exchange_rate;
   uint32_t max_recursion;
   uint32_t current_recursion = 0;
};

vector< fc::variant > database_api_impl::get_required_fees( const vector<operation>& ops, asset_id_type id )const
{
   vector< operation > _ops = ops;
   //
   // we copy the ops because we need to mutate an operation to reliably
   // determine its fee, see #435
   //

   vector< fc::variant > result;
   result.reserve(ops.size());
   const asset_object& a = id(_db);
   get_required_fees_helper helper(
      _db.current_fee_schedule(),
      a.options.core_exchange_rate,
      GET_REQUIRED_FEES_MAX_RECURSION );
   for( operation& op : _ops )
   {
      result.push_back( helper.set_op_fees( op ) );
   }
   return result;
}


vector<proposal_object> database_api_impl::get_proposed_transactions( account_id_type id )const
{
   const auto& idx = _db.get_index_type<proposal_index>();
   vector<proposal_object> result;

   idx.inspect_all_objects( [&](const object& obj){
           const proposal_object& p = static_cast<const proposal_object&>(obj);
           if( p.required_active_approvals.find( id ) != p.required_active_approvals.end() )
              result.push_back(p);
           else if ( p.required_owner_approvals.find( id ) != p.required_owner_approvals.end() )
              result.push_back(p);
           else if ( p.available_active_approvals.find( id ) != p.available_active_approvals.end() )
              result.push_back(p);
   });
   return result;
}

vector<blinded_balance_object> database_api_impl::get_blinded_balances( const flat_set<commitment_type>& commitments )const
{
   vector<blinded_balance_object> result; result.reserve(commitments.size());
   const auto& bal_idx = _db.get_index_type<blinded_balance_index>();
   const auto& by_commitment_idx = bal_idx.indices().get<by_commitment>();
   for( const auto& c : commitments )
   {
      auto itr = by_commitment_idx.find( c );
      if( itr != by_commitment_idx.end() )
         result.push_back( *itr );
   }
   return result;
}

void database_api_impl::broadcast_data_transaction_updates(const fc::variant& update)
{
   if (_data_transaction_subscribe_callback) {
      auto capture_this = shared_from_this();
      fc::async([capture_this,update](){
          if(capture_this->_data_transaction_subscribe_callback)
            capture_this->_data_transaction_subscribe_callback(update);
      });
   }

   if (_data_transaction_products_subscribe_callback) {
      auto capture_this = shared_from_this();
      fc::async([capture_this,update](){
          if(capture_this->_data_transaction_products_subscribe_callback)
            capture_this->_data_transaction_products_subscribe_callback(update);
      });
   }
}

void database_api_impl::broadcast_updates( const vector<variant>& updates )
{
   if( updates.size() && _subscribe_callback ) {
      auto capture_this = shared_from_this();
      fc::async([capture_this,updates](){
          if(capture_this->_subscribe_callback)
            capture_this->_subscribe_callback( fc::variant(updates) );
      });
   }
}

void database_api_impl::broadcast_market_updates( const market_queue_type& queue)
{
   if( queue.size() )
   {
      auto capture_this = shared_from_this();
      fc::async([capture_this, this, queue](){
          for( const auto& item : queue )
          {
            auto sub = _market_subscriptions.find(item.first);
            if( sub != _market_subscriptions.end() )
                sub->second( fc::variant(item.second ) );
          }
      });
   }
}

void database_api_impl::on_objects_removed( const vector<object_id_type>& ids, const vector<const object*>& objs, const flat_set<account_id_type>& impacted_accounts)
{
   handle_object_changed(_notify_remove_create, false, ids, impacted_accounts,
      [objs](object_id_type id) -> const object* {
         auto it = std::find_if(
               objs.begin(), objs.end(),
               [id](const object* o) {return o != nullptr && o->id == id;});

         if (it != objs.end())
            return *it;

         return nullptr;
      }
   );
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
// Data Market Interface                                            //
//                                                                  //
//////////////////////////////////////////////////////////////////////

/**
 *
 * 自定义排序函数，根据排序号进行排序
 * 注意：本函数的参数的类型一定要与vector中元素的类型一致
 * @brief sortByOrderNum
 * @param v1
 * @param v2
 * @return
 */
bool sortByOrderNum(const data_market_category_object &v1, const data_market_category_object &v2)
{
    return v1.order_num < v2.order_num; // asce sort
}

template<typename T>
vector<T> paging(vector<T> vector_temp,uint32_t offset,uint32_t limit){
    vector<T> vector_result;
    int end_index = offset+limit-1;
    if(end_index<0){
        end_index = 0;
    }

    if(vector_temp.size()<offset){
        vector_result = vector_temp;
    }else{
        uint32_t num = 0;
        typename vector<T>::iterator v;
        v = vector_temp.begin();
        std::advance(v,offset);
        for(;v!=vector_temp.end();v++){
            if(num==limit){
                break;
            }else{
                vector_result.push_back(*v);
                ++num;
            }
        }
    }
    return vector_result;
}

void database_api_impl::on_objects_new(const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts)
{
   handle_object_changed(_notify_remove_create, true, ids, impacted_accounts,
      std::bind(&object_database::find_object, &_db, std::placeholders::_1)
   );
}

void database_api_impl::on_objects_changed(const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts)
{
   handle_object_changed(false, true, ids, impacted_accounts,
      std::bind(&object_database::find_object, &_db, std::placeholders::_1)
   );
}

void database_api_impl::on_data_transaction_objects_changed(const string& request_id)
{
    // data_transaction callback
    if (_data_transaction_subscribe_callback) {
        auto obj = get_data_transaction_by_request_id(request_id);
        if (obj.valid()) {
            broadcast_data_transaction_updates(obj->to_variant());
        } else {
            dlog("get no data_transaction_object, request_id ${r}", ("r", request_id));
        }
    }

    // enhanced data_transaroction callback
    if (_data_transaction_products_subscribe_callback && !_data_transaction_subscribe_products.empty()) {
        auto obj = get_data_transaction_by_request_id(request_id);
        if (obj.valid()) {
            auto& products = _data_transaction_subscribe_products;
            auto iter = std::find(products.begin(), products.end(), obj->product_id);
            if (iter != products.end()) {
                broadcast_data_transaction_updates(obj->to_variant());
            } else {
                dlog("product_id ${p} not subscribed", ("p", obj->product_id));
            }
        } else {
            dlog("get no data_transaction_object, request_id ${r}", ("r", request_id));
        }
    }

}

void database_api_impl::handle_object_changed(bool force_notify, bool full_object, const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts, std::function<const object*(object_id_type id)> find_object)
{
   if( _subscribe_callback )
   {
      vector<variant> updates;

      for(auto id : ids)
      {
         if( force_notify || is_subscribed_to_item(id) || is_impacted_account(impacted_accounts) )
         {
            if( full_object )
            {
               auto obj = find_object(id);
               if( obj )
               {
                  updates.emplace_back( obj->to_variant() );
               }
            }
            else
            {
               updates.emplace_back( id );
            }
         }
      }

      broadcast_updates(updates);
   }

   if( _market_subscriptions.size() )
   {
      market_queue_type broadcast_queue;

      for(auto id : ids)
      {
         if( id.is<call_order_object>() )
         {
            enqueue_if_subscribed_to_market<call_order_object>( find_object(id), broadcast_queue, full_object );
         }
         else if( id.is<limit_order_object>() )
         {
            enqueue_if_subscribed_to_market<limit_order_object>( find_object(id), broadcast_queue, full_object );
         }
      }

      broadcast_market_updates(broadcast_queue);
   }
}


/** note: this method cannot yield because it is called in the middle of
 * apply a block.
 */
void database_api_impl::on_applied_block()
{
   if (_block_applied_callback)
   {
      auto capture_this = shared_from_this();
      block_id_type block_id = _db.head_block_id();
      fc::async([this,capture_this,block_id](){
         _block_applied_callback(fc::variant(block_id));
      });
   }

   if(_market_subscriptions.size() == 0)
      return;

   const auto& ops = _db.get_applied_operations();
   map< std::pair<asset_id_type,asset_id_type>, vector<pair<operation, operation_result>> > subscribed_markets_ops;
   for(const optional< operation_history_object >& o_op : ops)
   {
      if( !o_op.valid() )
         continue;
      const operation_history_object& op = *o_op;

      std::pair<asset_id_type,asset_id_type> market;
      switch(op.op.which())
      {
         /*  This is sent via the object_changed callback
         case operation::tag<limit_order_create_operation>::value:
            market = op.op.get<limit_order_create_operation>().get_market();
            break;
         */
         case operation::tag<fill_order_operation>::value:
            market = op.op.get<fill_order_operation>().get_market();
            break;
            /*
         case operation::tag<limit_order_cancel_operation>::value:
         */
         default: break;
      }
      if(_market_subscriptions.count(market))
         subscribed_markets_ops[market].push_back(std::make_pair(op.op, op.result));
   }
   /// we need to ensure the database_api is not deleted for the life of the async operation
   auto capture_this = shared_from_this();
   fc::async([this,capture_this,subscribed_markets_ops](){
      for(auto item : subscribed_markets_ops)
      {
         auto itr = _market_subscriptions.find(item.first);
         if(itr != _market_subscriptions.end())
            itr->second(fc::variant(item.second));
      }
   });
}


uint64_t database_api_impl::get_test( ) const
{
   return 100;
}


vector<free_data_product_object> database_api_impl::list_home_free_data_products(uint8_t limit) const {
    FC_ASSERT(limit<30 );
    free_data_product_search_results_object  srs;
    vector<free_data_product_object>  free_vecotr_result;
    vector<free_data_product_object> free_vecotr_result_temp;
    const free_data_product_index& product_index = _db.get_index_type<free_data_product_index>();
    auto range = product_index.indices().get<by_recommend_expiration_date_time>().equal_range(boost::make_tuple(fc::time_point_sec::maximum()));

    for (const free_data_product_object& free_data_product : boost::make_iterator_range(range.first, range.second))
    {
        if(free_data_product.status==data_product_enable_status){
            free_vecotr_result_temp.push_back(free_data_product);
        }
    }

    if(limit>free_vecotr_result_temp.size()){
        limit = free_vecotr_result_temp.size();
    }
    for(int i = 0 ; i < limit ; i ++){
        free_vecotr_result.push_back(free_vecotr_result_temp[i]);
    }

    return free_vecotr_result;
}

vector<league_object> database_api_impl::list_home_leagues(uint8_t limit) const {
    FC_ASSERT(limit<30);
    league_search_results_object  srs;
    vector<league_object> home_leagues_result;
    vector<league_object> home_leagues_result_temp;
    const league_index& product_index = _db.get_index_type<league_index>();
    auto range = product_index.indices().get<by_recommend_expiration_date_time>().equal_range(boost::make_tuple(fc::time_point_sec::maximum()));

    for (const league_object& league : boost::make_iterator_range(range.first, range.second)) {
        if(league.status==data_product_enable_status){
            home_leagues_result_temp.push_back(league);
        }

    }

    if(limit>home_leagues_result_temp.size()) {
        limit = home_leagues_result_temp.size();
    }
    for(uint8_t i = 0; i < limit && i < home_leagues_result_temp.size(); i++) {
        home_leagues_result.push_back(home_leagues_result_temp[i]);
    }
    return home_leagues_result;
}

vector<data_market_category_object> database_api_impl::list_data_market_categories(uint8_t data_market_type) const {
    vector<data_market_category_object>  dmcver;

    FC_ASSERT( data_market_type == 1 || data_market_type ==2, "the data_market_type does not support" );

    const data_market_category_index& category_index = _db.get_index_type<data_market_category_index>();
    auto range = category_index.indices().get<by_data_market_type>().equal_range(boost::make_tuple(data_market_type));
    for (const data_market_category_object& category : boost::make_iterator_range(range.first, range.second))
    {
        dmcver.push_back(category);
    }

   // sort
   //std::cout<<"dmcver:"<<dmcver.size()<<std::endl;
   std::sort(dmcver.begin(),dmcver.end(),sortByOrderNum);


    return dmcver;
}

free_data_product_search_results_object   database_api_impl::list_free_data_products(string data_market_category_id,uint32_t offset,uint32_t limit,string order_by,string keyword,bool show_all) const{
    FC_ASSERT( limit <= 100 );
    free_data_product_search_results_object  srs;
    vector<free_data_product_object>  free_vecotr_result;
    vector<free_data_product_object> free_vecotr_result_temp;

    const free_data_product_index& product_index = _db.get_index_type<free_data_product_index>();
    data_market_category_id_type  category_id = fc::variant(data_market_category_id).as<data_market_category_id_type>();
    auto range = product_index.indices().get<by_data_market_category_id>().equal_range(boost::make_tuple(category_id));

   // const auto& range = _db.get_index_type<free_data_product_index>().indices().get<by_id>();
    int total = 0;
    for (const free_data_product_object& free_data_product : boost::make_iterator_range(range.first, range.second))
    {

        keyword = trim_copy(keyword);
        if(keyword.empty()){
            if(show_all){
                 free_vecotr_result_temp.push_back(free_data_product);
            }else{
                if(free_data_product.status>data_product_undo_status){
                    free_vecotr_result_temp.push_back(free_data_product);
                }
            }

        }else{
            if(contains(free_data_product.product_name, keyword)  ||  contains(free_data_product.brief_desc, keyword) ){
                if(show_all){
                    free_vecotr_result_temp.push_back(free_data_product);
                }else{
                    if(free_data_product.status>data_product_undo_status){
                        free_vecotr_result_temp.push_back(free_data_product);
                    }
                }

            }
        }
         ++total;
    }

    // paging
    free_vecotr_result = paging(free_vecotr_result_temp,offset,limit);
    srs.data = free_vecotr_result;
    srs.filtered_total = free_vecotr_result_temp.size();
    srs.total = total;
    return srs;
}

league_data_product_search_results_object   database_api_impl::list_league_data_products(string data_market_category_id,uint32_t offset,uint32_t limit,string order_by,string keyword,bool show_all) const{
    FC_ASSERT( limit <= 100 );
    league_data_product_search_results_object  srs;
    vector<league_data_product_object>  league_vecotr_result;
    vector<league_data_product_object> league_vecotr_result_temp;

    const league_data_product_index& product_index = _db.get_index_type<league_data_product_index>();
    data_market_category_id_type  category_id = fc::variant(data_market_category_id).as<data_market_category_id_type>();
    auto range = product_index.indices().get<by_data_market_category_id>().equal_range(boost::make_tuple(category_id));
    int total = 0;
    for (const league_data_product_object& league_data_product : boost::make_iterator_range(range.first, range.second))
    {
        keyword = trim_copy(keyword);
        if(keyword.empty()){
            if(show_all){
                league_vecotr_result_temp.push_back(league_data_product);
            }else{
                if(league_data_product.status>league_undo_status){
                    league_vecotr_result_temp.push_back(league_data_product);
                }
            }
        }else{
            if(contains(league_data_product.product_name, keyword)  ||  contains(league_data_product.brief_desc, keyword) ){
                if(show_all){
                    league_vecotr_result_temp.push_back(league_data_product);
                }else{
                    if(league_data_product.status>data_product_undo_status){
                        league_vecotr_result_temp.push_back(league_data_product);
                    }
                }
            }
            ++total;
        }
    }

    // pagine
    league_vecotr_result = paging(league_vecotr_result_temp,offset,limit);
    srs.data = league_vecotr_result;
    srs.filtered_total = league_vecotr_result.size();
    srs.total = total;
    return srs;

}

data_transaction_search_results_object database_api_impl::list_data_transactions_by_requester(string requester, uint32_t limit) const {
    // FC_ASSERT(limit <= 100);
    data_transaction_search_results_object search_result;
    return search_result;
}

map<account_id_type, uint64_t> database_api_impl::list_second_hand_datasources(time_point_sec start_date_time, time_point_sec end_date_time, uint32_t limit) const {
    const auto& idx = _db.get_index_type<second_hand_data_index>().indices().get<by_create_date_time>();
    auto range = idx.equal_range(boost::make_tuple(start_date_time, end_date_time));

    std::map<account_id_type, uint64_t> datasource_accounts;
    for (const second_hand_data_object& obj : boost::make_iterator_range(range.first, range.second)) {
        datasource_accounts[obj.second_hand_datasource_id] = 0;
    }
    for (const second_hand_data_object& obj : boost::make_iterator_range(range.first, range.second)) {
        ++datasource_accounts[obj.second_hand_datasource_id];
    }

    // sort
    vector<PAIR> datasource_vec(datasource_accounts.begin(), datasource_accounts.end());
    sort(datasource_vec.begin(), datasource_vec.end(), cmp_pair_by_value());

    std::map<account_id_type, uint64_t> results;
    for (int i = 0; i < limit && i < datasource_vec.size(); ++i) {
        results.insert(datasource_vec.at(i));
    }

    return results;
}

uint32_t database_api_impl::list_total_second_hand_transaction_counts_by_datasource(fc::time_point_sec start_date_time, fc::time_point_sec end_date_time, account_id_type datasource_account) const {
    return _db.get_index_type<second_hand_data_index>().indices().size();
 }

optional<data_transaction_object> database_api_impl::get_data_transaction_by_request_id(string request_id) const {
    vector<data_transaction_object> result;

    const auto& dt_idx = _db.get_index_type<data_transaction_index>();
    auto range = dt_idx.indices().get<by_request_id>().equal_range(request_id);
    for (const auto& obj : boost::make_iterator_range(range.first, range.second)) {
        result.push_back(obj);
    }

    if (!result.empty()) {
        return result.back();
    }
    return {};
}

league_search_results_object database_api_impl::list_leagues(string data_market_category_id,uint32_t offset,uint32_t limit,string order_by,string keyword,bool show_all) const {
    FC_ASSERT( limit <= 100 );
    league_search_results_object  search_result;
    vector<league_object>  league_vecotr_result;
    vector<league_object> league_vecotr_result_temp;

    const league_index& product_index = _db.get_index_type<league_index>();
    data_market_category_id_type category_id = fc::variant(data_market_category_id).as<data_market_category_id_type>();
    auto range = product_index.indices().get<by_data_market_category_id>().equal_range(boost::make_tuple(category_id));
    int total = 0;
    for (const league_object& league : boost::make_iterator_range(range.first, range.second))
    {
        keyword = trim_copy(keyword);
        if(keyword.empty()){
            if(show_all){
                league_vecotr_result_temp.push_back(league);
            }else{
                if(league.status>league_undo_status){
                    league_vecotr_result_temp.push_back(league);
                }
            }
        }
        else {
            if(contains(league.league_name, keyword) || contains(league.brief_desc, keyword) ){
                if(show_all){
                    league_vecotr_result_temp.push_back(league);
                }else{
                    if(league.status>league_undo_status){
                        league_vecotr_result_temp.push_back(league);
                    }
                }
            }
        }
        ++total;
    }

    //sort
    std::sort(league_vecotr_result_temp.begin(),league_vecotr_result_temp.end(), sort_league_object_by_name());

    //paging result
    league_vecotr_result = paging(league_vecotr_result_temp,offset,limit);
    search_result.data = league_vecotr_result;
    search_result.filtered_total = league_vecotr_result.size();
    search_result.total = total;
    return search_result;
}


vector<optional<data_market_category_object>> database_api_impl::get_data_market_categories(const vector<data_market_category_id_type>& data_market_category_ids)const{
    vector<optional<data_market_category_object>> result;
    result.reserve(data_market_category_ids.size());
    std::transform(data_market_category_ids.begin(), data_market_category_ids.end(), std::back_inserter(result),
                   [this](data_market_category_id_type id) -> optional<data_market_category_object> {
       if(auto o = _db.find(id))
          return *o;
       return {};
    });
    return result;
}


vector<optional<free_data_product_object>> database_api_impl::get_free_data_products(const vector<free_data_product_id_type>& free_data_product_ids)const{
    vector<optional<free_data_product_object>> result;
    result.reserve(free_data_product_ids.size());
    std::transform(free_data_product_ids.begin(), free_data_product_ids.end(), std::back_inserter(result),
                   [this](free_data_product_id_type id) -> optional<free_data_product_object> {
       if(auto o = _db.find(id))
          return *o;
       return {};
    });
    return result;
}

vector<optional<league_data_product_object>> database_api_impl::get_league_data_products(const vector<league_data_product_id_type>& league_data_product_ids)const{
    vector<optional<league_data_product_object>> result;
    result.reserve(league_data_product_ids.size());
    std::transform(league_data_product_ids.begin(), league_data_product_ids.end(), std::back_inserter(result),
                   [this](league_data_product_id_type id) -> optional<league_data_product_object> {
       if(auto o = _db.find(id))
          return *o;
       return {};
    });
    return result;
}

vector<optional<league_object>> database_api_impl::get_leagues(const vector<league_id_type>& league_ids) const {
    vector<optional<league_object>> result;
    result.reserve(league_ids.size());
    std::transform(league_ids.begin(), league_ids.end(), std::back_inserter(result),
                   [this](league_id_type id) -> optional<league_object> {
       if(auto o = _db.find(id))
          return *o;
       return {};
    });
    return result;
}

uint32_t database_api_impl::get_witness_participation_rate() const
{
    return _db.witness_participation_rate();
}

} } // graphene::app
