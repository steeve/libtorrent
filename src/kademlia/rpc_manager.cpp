/*

Copyright (c) 2006, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/pch.hpp"
#include "libtorrent/socket.hpp"

#include <boost/bind.hpp>
#include <boost/mpl/max_element.hpp>
#include <boost/mpl/vector.hpp>
#include <boost/mpl/sizeof.hpp>
#include <boost/mpl/transform_view.hpp>
#include <boost/mpl/deref.hpp>

#include <libtorrent/io.hpp>
#include <libtorrent/invariant_check.hpp>
#include <libtorrent/kademlia/rpc_manager.hpp>
#include <libtorrent/kademlia/logging.hpp>
#include <libtorrent/kademlia/routing_table.hpp>
#include <libtorrent/kademlia/find_data.hpp>
#include <libtorrent/kademlia/refresh.hpp>
#include <libtorrent/kademlia/node.hpp>
#include <libtorrent/kademlia/observer.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/time.hpp>

#include <fstream>

using boost::shared_ptr;
using boost::bind;

namespace libtorrent { namespace dht
{

namespace io = libtorrent::detail;
namespace mpl = boost::mpl;

#ifdef TORRENT_DHT_VERBOSE_LOGGING
TORRENT_DEFINE_LOG(rpc)
#endif

void intrusive_ptr_add_ref(observer const* o)
{
	TORRENT_ASSERT(o->m_refs >= 0);
	TORRENT_ASSERT(o != 0);
	++o->m_refs;
}

void intrusive_ptr_release(observer const* o)
{
	TORRENT_ASSERT(o->m_refs > 0);
	TORRENT_ASSERT(o != 0);
	if (--o->m_refs == 0)
	{
		boost::pool<>& p = o->m_algorithm->allocator();
		(const_cast<observer*>(o))->~observer();
		p.free(const_cast<observer*>(o));
	}
}

void observer::set_target(udp::endpoint const& ep)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	// use high resolution timers for logging
	m_sent = time_now_hires();
#else
	m_sent = time_now();
#endif

	m_port = ep.port();
#if TORRENT_USE_IPV6
	if (ep.address().is_v6())
	{
		m_is_v6 = true;
		m_addr.v6 = ep.address().to_v6().to_bytes();
	}
	else
#endif
	{
		m_is_v6 = false;
		m_addr.v4 = ep.address().to_v4().to_bytes();
	}
}

address observer::target_addr() const
{
#if TORRENT_USE_IPV6
	if (m_is_v6)
		return address_v6(m_addr.v6);
	else
#endif
		return address_v4(m_addr.v4);
}

udp::endpoint observer::target_ep() const
{
	return udp::endpoint(target_addr(), m_port);
}

void observer::abort()
{
	if (m_done) return;
	m_done = true;
	m_algorithm->failed(target_ep(), traversal_algorithm::prevent_request);
}

void observer::done()
{
	if (m_done) return;
	m_done = true;
	m_algorithm->finished(target_ep());
}

void observer::short_timeout()
{
	if (m_short_timeout) return;
	TORRENT_ASSERT(m_short_timeout == false);
	m_short_timeout = true;
	m_algorithm->failed(target_ep(), traversal_algorithm::short_timeout);
}

// this is called when no reply has been received within
// some timeout
void observer::timeout()
{
	if (m_done) return;
	m_done = true;
	m_algorithm->failed(target_ep());
}

node_id generate_id();

typedef mpl::vector<
	find_data_observer
	, announce_observer
	, null_observer
	> observer_types;

typedef mpl::max_element<
	mpl::transform_view<observer_types, mpl::sizeof_<mpl::_1> >
    >::type max_observer_type_iter;

rpc_manager::rpc_manager(node_id const& our_id
	, routing_table& table, send_fun const& sf
	, void* userdata)
	: m_pool_allocator(sizeof(mpl::deref<max_observer_type_iter::base>::type), 10)
	, m_next_transaction_id(std::rand() % max_transaction_id)
	, m_send(sf)
	, m_userdata(userdata)
	, m_our_id(our_id)
	, m_table(table)
	, m_timer(time_now())
	, m_random_number(generate_id())
	, m_destructing(false)
{
	std::srand(time(0));

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(rpc) << "Constructing";

#define PRINT_OFFSETOF(x, y) TORRENT_LOG(rpc) << "  +" << offsetof(x, y) << ": " #y

	TORRENT_LOG(rpc) << " observer: " << sizeof(observer);
	PRINT_OFFSETOF(observer, m_sent);
	PRINT_OFFSETOF(observer, m_refs);
	PRINT_OFFSETOF(observer, m_algorithm);
	PRINT_OFFSETOF(observer, m_addr);
	PRINT_OFFSETOF(observer, m_port);
	PRINT_OFFSETOF(observer, m_transaction_id);

	TORRENT_LOG(rpc) << " announce_observer: " << sizeof(announce_observer);
	TORRENT_LOG(rpc) << " null_observer: " << sizeof(null_observer);
	TORRENT_LOG(rpc) << " find_data_observer: " << sizeof(find_data_observer);

	TORRENT_LOG(rpc) << " traversal_algorithm::result: " << sizeof(traversal_algorithm::result);
	PRINT_OFFSETOF(traversal_algorithm::result, id);
	PRINT_OFFSETOF(traversal_algorithm::result, addr);
	PRINT_OFFSETOF(traversal_algorithm::result, port);
	PRINT_OFFSETOF(traversal_algorithm::result, flags);

#undef PRINT_OFFSETOF
#endif

}

rpc_manager::~rpc_manager()
{
	TORRENT_ASSERT(!m_destructing);
	m_destructing = true;
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(rpc) << "Destructing";
#endif
	
	for (transactions_t::iterator i = m_transactions.begin()
		, end(m_transactions.end()); i != end; ++i)
	{
		(*i)->abort();
	}
}

#ifdef TORRENT_DEBUG
size_t rpc_manager::allocation_size() const
{
	size_t s = sizeof(mpl::deref<max_observer_type_iter::base>::type);
	return s;
}

void rpc_manager::check_invariant() const
{
	TORRENT_ASSERT(m_next_transaction_id >= 0);
	TORRENT_ASSERT(m_next_transaction_id < max_transaction_id);

	for (transactions_t::const_iterator i = m_transactions.begin()
		, end(m_transactions.end()); i != end; ++i)
	{
		TORRENT_ASSERT(*i);
	}
}
#endif

void rpc_manager::unreachable(udp::endpoint const& ep)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(rpc) << time_now_string() << " PORT_UNREACHABLE [ ip: " << ep << " ]";
#endif

	for (transactions_t::iterator i = m_transactions.begin();
		i != m_transactions.end();)
	{
		TORRENT_ASSERT(*i);
		observer_ptr const& o = *i;
		if (o->target_ep() != ep) { ++i; continue; }
		observer_ptr ptr = *i;
		m_transactions.erase(i++);
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		TORRENT_LOG(rpc) << "  found transaction [ tid: " << ptr->transaction_id() << " ]";
#endif
		ptr->timeout();
		break;
	}
}

// defined in node.cpp
void incoming_error(entry& e, char const* msg);

bool rpc_manager::incoming(msg const& m)
{
	INVARIANT_CHECK;

	if (m_destructing) return false;

	// we only deal with replies, not queries
	TORRENT_ASSERT(m.message.dict_find_string_value("y") == "r");

	// if we don't have the transaction id in our
	// request list, ignore the packet

	std::string transaction_id = m.message.dict_find_string_value("t");

	std::string::const_iterator i = transaction_id.begin();	
	int tid = transaction_id.size() != 2 ? -1 : io::read_uint16(i);

	observer_ptr o;

	for (transactions_t::iterator i = m_transactions.begin()
		, end(m_transactions.end()); i != end; ++i)
	{
		TORRENT_ASSERT(*i);
		if ((*i)->transaction_id() != tid) continue;
		if (m.addr.address() != (*i)->target_addr()) continue;
		o = *i;
		m_transactions.erase(i);
		break;
	}

	if (!o)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		TORRENT_LOG(rpc) << "Reply with invalid transaction id size: " 
			<< transaction_id.size() << " from " << m.addr;
#endif
		entry e;
		incoming_error(e, "invalid transaction id");
		m_send(m_userdata, e, m.addr, 0);
		return false;
	}

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	std::ofstream reply_stats("round_trip_ms.log", std::ios::app);
	reply_stats << m.addr << "\t" << total_milliseconds(time_now_hires() - o->sent())
		<< std::endl;
#endif

	lazy_entry const* ret_ent = m.message.dict_find_dict("r");
	if (ret_ent == 0)
	{
		entry e;
		incoming_error(e, "missing 'r' key");
		m_send(m_userdata, e, m.addr, 0);
		return false;
	}

	lazy_entry const* node_id_ent = ret_ent->dict_find_string("id");
	if (node_id_ent == 0 || node_id_ent->string_length() != 20)
	{
		entry e;
		incoming_error(e, "missing 'id' key");
		m_send(m_userdata, e, m.addr, 0);
		return false;
	}

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(rpc) << "[" << o->m_algorithm.get() << "] Reply with transaction id: " 
		<< tid << " from " << m.addr;
#endif
	o->reply(m);
	return m_table.node_seen(node_id(node_id_ent->string_ptr()), m.addr);
}

time_duration rpc_manager::tick()
{
	INVARIANT_CHECK;

	const static int short_timeout = 3;
	const static int timeout = 20;

	//	look for observers that have timed out

	if (m_transactions.empty()) return seconds(short_timeout);

	std::list<observer_ptr> timeouts;

	time_duration ret = seconds(short_timeout);
	ptime now = time_now();

	for (transactions_t::iterator i = m_transactions.begin();
		i != m_transactions.end();)
	{
		observer_ptr o = *i;

		// if we reach an observer that hasn't timed out
		// break, because every observer after this one will
		// also not have timed out yet
		time_duration diff = now - o->sent();
		if (diff < seconds(timeout))
		{
			ret = seconds(timeout) - diff;
			break;
		}
		
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		TORRENT_LOG(rpc) << "[" << o->m_algorithm.get() << "] Timing out transaction id: " 
			<< (*i)->transaction_id() << " from " << o->target_ep();
#endif
		m_transactions.erase(i++);
		timeouts.push_back(o);
	}
	
	std::for_each(timeouts.begin(), timeouts.end(), bind(&observer::timeout, _1));
	timeouts.clear();

	for (transactions_t::iterator i = m_transactions.begin();
		i != m_transactions.end(); ++i)
	{
		observer_ptr o = *i;

		// if we reach an observer that hasn't timed out
		// break, because every observer after this one will
		// also not have timed out yet
		time_duration diff = now - o->sent();
		if (diff < seconds(short_timeout))
		{
			ret = seconds(short_timeout) - diff;
			break;
		}
		
		if (o->has_short_timeout()) continue;

		// TODO: don't call short_timeout() again if we've
		// already called it once
		timeouts.push_back(o);
	}

	std::for_each(timeouts.begin(), timeouts.end(), bind(&observer::short_timeout, _1));
	
	return ret;
}

void rpc_manager::add_our_id(entry& e)
{
	e["id"] = m_our_id.to_string();
}

bool rpc_manager::invoke(entry& e, udp::endpoint target_addr
	, observer_ptr o)
{
	INVARIANT_CHECK;

	if (m_destructing) return false;

	e["y"] = "q";
	entry& a = e["a"];
	add_our_id(a);

	std::string transaction_id;
	transaction_id.resize(2);
	char* out = &transaction_id[0];
	io::write_uint16(m_next_transaction_id, out);
	e["t"] = transaction_id;
		
	o->set_target(target_addr);
	o->set_transaction_id(m_next_transaction_id);

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(rpc) << "[" << o->m_algorithm.get() << "] invoking "
		<< e["q"].string() << " -> " << target_addr;
#endif

	if (m_send(m_userdata, e, target_addr, 1))
	{
		m_transactions.push_back(o);
		++m_next_transaction_id;
  		m_next_transaction_id %= max_transaction_id;
#ifdef TORRENT_DEBUG
		o->m_was_sent = true;
#endif
	}
	return true;
}

observer::~observer()
{
	// if the message was sent, it must have been
	// reported back to the traversal_algorithm as
	// well. If it wasn't sent, it cannot have been
	// reported back
	TORRENT_ASSERT(m_was_sent == m_done);
	TORRENT_ASSERT(!m_in_constructor);
}

} } // namespace libtorrent::dht

