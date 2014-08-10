/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * 2012+ Copyright (c) Ruslan Nigmatullin <euroelessar@yandex.ru>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#ifndef CALLBACK_P_H
#define CALLBACK_P_H

#include "elliptics/cppdef.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <blackhole/scoped_attributes.hpp>

#include "elliptics/async_result_cast.hpp"

extern "C" {
#include "foreign/cmp/cmp.h"
}

//#ifdef DEVELOPER_BUILD
//#  define elliptics_assert(expr) assert(expr)
//#else
#  define elliptics_assert(expr)
//#endif

namespace ioremap { namespace elliptics {

class session_scope
{
	public:
		session_scope(session &sess) : m_sess(sess)
		{
			m_filter = m_sess.get_filter();
			m_checker = m_sess.get_checker();
			m_policy = m_sess.get_exceptions_policy();
			m_cflags = m_sess.get_cflags();
			m_ioflags = m_sess.get_ioflags();
		}

		~session_scope()
		{
			m_sess.set_filter(m_filter);
			m_sess.set_checker(m_checker);
			m_sess.set_exceptions_policy(m_policy);
			m_sess.set_cflags(m_cflags);
			m_sess.set_ioflags(m_ioflags);
		}

	private:
		session &m_sess;
		result_filter m_filter;
		result_checker m_checker;
		uint64_t m_cflags;
		uint32_t m_ioflags;
		uint32_t m_policy;
};

class scoped_trace_id
{
public:
	scoped_trace_id(session &sess) :
		m_attributes(sess.get_logger(), create_attributes(sess))
	{
	}

private:
	static blackhole::log::attributes_t create_attributes(session &sess)
	{
		blackhole::log::attributes_t attributes = {
			keyword::request_id() = sess.get_trace_id()
		};
		return std::move(attributes);
	}

	blackhole::scoped_attributes_t m_attributes;
};

typedef int (*complete_func)(struct dnet_net_state *, struct dnet_cmd *, void *);

class callback_result_data
{
	public:
		callback_result_data()
		{
		}

		callback_result_data(dnet_addr *addr, dnet_cmd *cmd)
		{
			const size_t size = sizeof(dnet_addr) + sizeof(dnet_cmd) + cmd->size;
			data = data_pointer::allocate(size);
			if (addr)
				memcpy(data.data(), addr, sizeof(dnet_addr));
			else
				memset(data.data(), 0, sizeof(dnet_addr));
			memcpy(data.data<char>() + sizeof(dnet_addr), cmd, sizeof(dnet_cmd) + cmd->size);
		}

		virtual ~callback_result_data()
		{
		}

		data_pointer data;
		error_info error;
		exec_context context;
};

enum special_count { unlimited };

struct entry_converter
{
	static void convert(exec_result_entry &entry, callback_result_data *data)
	{
		data->context = exec_context::parse(entry.data(), &data->error);
	}

	static void convert(iterator_result_entry &entry, callback_result_data *)
	{
		dnet_convert_iterator_response(entry.reply());
	}

	static void convert(lookup_result_entry &entry, callback_result_data *)
	{
		dnet_convert_addr(entry.storage_address());
		dnet_convert_file_info(entry.file_info());
	}

	static void convert(read_result_entry &entry, callback_result_data *)
	{
		dnet_convert_io_attr(entry.io_attribute());
	}

	static void convert(stat_result_entry &entry, callback_result_data *)
	{
		dnet_convert_stat(entry.statistics());
	}

	static void convert(stat_count_result_entry &entry, callback_result_data *)
	{
		dnet_convert_addr_stat(entry.statistics(), 0);
	}

	static void convert(backend_status_result_entry &, callback_result_data *)
	{
	}

	static void convert(callback_result_entry &, callback_result_data *)
	{
	}
};

struct dnet_net_state_deleter
{
	void operator () (dnet_net_state *state) const
	{
		if (state)
			dnet_state_put(state);
	}
};

typedef std::unique_ptr<dnet_net_state, dnet_net_state_deleter> net_state_ptr;

// Send request to specific state
async_generic_result send_to_single_state(session &sess, const transport_control &control);
async_generic_result send_to_single_state(session &sess, dnet_io_control &control);

// Send request to each backend
async_generic_result send_to_each_backend(session &sess, const transport_control &control);

// Send request to one state at each session's group
async_generic_result send_to_groups(session &sess, const transport_control &control);
async_generic_result send_to_groups(session &sess, dnet_io_control &control);

// Send request to each state in route table
async_generic_result send_to_each_node(session &sess, const transport_control &control);

template <typename T>
class default_callback
{
	public:
		typedef std::function<void (const T &)> entry_processor_func;

		default_callback(const session &sess, const async_result<T> &result)
			: m_logger(sess.get_logger()),
			  m_count(1), m_complete(0), m_result(result), m_proto_error(false)
		{
		}

		virtual ~default_callback()
		{
		}

		bool set_count(size_t count)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_count = count;
			return m_count == m_complete;
		}

		void set_count(special_count)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_count = static_cast<size_t>(-1);
		}

		void set_total(size_t total)
		{
			m_result.set_total(total);
		}

		size_t get_total() const
		{
			return m_result.get_total();
		}

		bool handle(struct dnet_net_state *state, struct dnet_cmd *cmd, complete_func, void *)
		{
			std::lock_guard<std::mutex> lock(m_mutex);

			if (is_trans_destroyed(state, cmd)) {
				++m_complete;
			} else {
				if (!(cmd->flags & DNET_FLAGS_MORE)) {
					if (m_proto_error && cmd->status == 0) {
						m_statuses.push_back(-EPROTO);
						m_proto_error = 0;
					} else {
						m_statuses.push_back(cmd->status);
					}
				}
				auto data = std::make_shared<callback_result_data>(state ? dnet_state_addr(state) : NULL, cmd);
				process(cmd, data, data.get());
			}
			return (m_count == m_complete);
		}

		void process(dnet_cmd *cmd, const callback_result_entry &default_entry, callback_result_data *data)
		{
			T entry = *static_cast<const T *>(&default_entry);
			if (cmd->status) {
				data->error = create_error(*cmd);
			}
			if (!entry.data().empty()) {
				try {
					entry_converter::convert(entry, data);
				} catch (...) {
					BH_LOG(m_logger, DNET_LOG_ERROR, "%s: received invalid data from server, tid: %llu, cmd: %s, status: %d, size: %llu",
						       dnet_dump_id(&cmd->id),
						       static_cast<unsigned long long>(cmd->trans),
						       dnet_cmd_string(cmd->cmd),
						       int(cmd->status),
						       static_cast<unsigned long long>(cmd->size));

					dnet_cmd *cmd_copy = default_entry.command();
					if (cmd_copy->status == 0)
						cmd_copy->status = -EPROTO;
					if (cmd_copy->flags & DNET_FLAGS_MORE) {
						m_proto_error = true;
					} else {
						m_statuses.back() = cmd_copy->status;
					}

					cmd_copy->flags &= ~DNET_FLAGS_MORE;
					cmd_copy->size = 0;

					data->data = data->data.slice(0, sizeof(dnet_addr) + sizeof(dnet_cmd));
				}
			}
			process(entry);
		}

		void process(const T &entry)
		{
			if (m_process_entry && entry.status() == 0 && !entry.data().empty()) {
				m_process_entry(entry);
			}

			m_result.process(entry);
		}

		void set_process_entry(const entry_processor_func &process_entry)
		{
			m_process_entry = process_entry;
		}

		bool is_ready()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return (m_count == m_complete);
		}

		bool is_valid() const
		{
			bool ok = false;
			for (size_t i = 0; i < m_statuses.size(); ++i)
				ok |= (m_statuses[i] == 0);
			return ok;
		}

		const std::vector<int> &statuses() const
		{
			return m_statuses;
		}

		void clear()
		{
			m_complete = 0;
			m_statuses.clear();
			m_proto_error = false;
		}

		void complete(std::exception_ptr exc)
		{
			if (exc) {
				try {
					std::rethrow_exception(exc);
				} catch (error &e) {
					m_result.complete(error_info(e.error_code(), e.error_message()));
				} catch (std::bad_alloc &) {
					m_result.complete(error_info(-ENOMEM, std::string()));
				}
			} else {
				m_result.complete(error_info());
			}
		}

		void complete(const error_info &error)
		{
			m_result.complete(error);
		}

	protected:
		logger &m_logger;
		size_t m_count;
		size_t m_complete;
		std::vector<int> m_statuses;
		std::mutex m_mutex;
		entry_processor_func m_process_entry;
		typename async_result<T>::handler m_result;
		bool m_proto_error;
};

template <typename Result, dnet_commands Command>
class base_stat_callback
{
	public:
		base_stat_callback(const session &sess, const async_result<Result> &result)
			: sess(sess), cb(sess, result), has_id(false)
		{
		}

		virtual ~base_stat_callback()
		{
		}

		bool start(error_info *error, complete_func func, void *priv)
		{
			cb.set_count(unlimited);

			uint64_t cflags_pop = sess.get_cflags();
			sess.set_cflags(cflags_pop | DNET_ATTR_CNTR_GLOBAL);
			int err = dnet_request_stat(sess.get_native(),
				has_id ? &id : NULL, Command, func, priv);
			sess.set_cflags(cflags_pop);

			if (err < 0) {
				*error = create_error(err, "Failed to request statistics");
				return true;
			}

			return cb.set_count(err);
		}

		bool handle(error_info *error, dnet_net_state *state, dnet_cmd *cmd, complete_func func, void *priv)
		{
			(void) error;
			return cb.handle(state, cmd, func, priv);
		}

		void finish(const error_info &exc)
		{
			cb.complete(exc);
		}

		dnet_commands command;
		session sess;
		default_callback<Result> cb;
		dnet_id id;
		bool has_id;
};

template <typename Handler, typename Entry>
class multigroup_handler : public std::enable_shared_from_this<multigroup_handler<Handler, Entry>>
{
public:
	typedef multigroup_handler<Handler, Entry> parent_type;

	multigroup_handler(const session &sess, const async_result<Entry> &result, std::vector<int> &&groups) :
		m_sess(sess.clean_clone()),
		m_handler(result),
		m_groups(std::move(groups)),
		m_group_index(0)
	{
		m_sess.set_checker(sess.get_checker());
	}

	void start()
	{
		if (m_groups.empty()) {
			m_handler.complete(error_info());
			return;
		}

		next_group();
	}

	void process(const Entry &entry)
	{
		process_entry(entry);

		m_handler.process(entry);
	}

	void complete(const error_info &error)
	{
		group_finished(error);
		++m_group_index;

		if (m_group_index < m_groups.size() && need_next_group(error)) {
			next_group();
		} else {
			m_handler.complete(error_info());
		}
	}

	void set_total(size_t total)
	{
		m_handler.set_total(total);
	}

protected:
	int current_group()
	{
		return m_groups[m_group_index];
	}

	void next_group()
	{
		using std::placeholders::_1;

		async_result_cast<Entry>(m_sess, send_to_next_group()).connect(
			std::bind(&multigroup_handler::process, this->shared_from_this(), _1),
			std::bind(&multigroup_handler::complete, this->shared_from_this(), _1)
		);
	}

	// Override this if you want to do something on each received packet
	virtual void process_entry(const Entry &entry)
	{
		(void) entry;
	}

	// Override this if you want to change the stop condition
	virtual bool need_next_group(const error_info &error)
	{
		return !!error;
	}

	// Override this if you want to do something before going to next group
	virtual void group_finished(const error_info &error)
	{
		(void) error;
	}

	// Override this to implement your send logic
	virtual async_generic_result send_to_next_group() = 0;

	session m_sess;
	async_result_handler<Entry> m_handler;
	const std::vector<int> m_groups;
	size_t m_group_index;
};

class net_state_id
{
public:
	net_state_id() : m_backend(-1)
	{
	}

	net_state_id(net_state_id &&other) : m_state(std::move(other.m_state)), m_backend(other.m_backend)
	{
	}

	net_state_id(dnet_node *node, const dnet_id *id) : m_backend(-1)
	{
		reset(node, id);
	}

	net_state_id &operator =(net_state_id &&other)
	{
		m_state = std::move(other.m_state);
		m_backend = other.m_backend;
		return *this;
	}

	void reset(dnet_node *node, const dnet_id *id)
	{
		m_backend = -1;
		m_state.reset(dnet_state_get_first_with_backend(node, id, &m_backend));
	}

	void reset()
	{
		m_state.reset();
		m_backend = -1;
	}

	dnet_net_state *operator ->() const
	{
		return m_state.get();
	}

	bool operator ==(const net_state_id &other)
	{
		return m_state == other.m_state && m_backend == other.m_backend;
	}

	bool operator !() const
	{
		return !m_state;
	}

	operator bool() const
	{
		return !!m_state;
	}

	dnet_net_state *state() const
	{
		return m_state.get();
	}

	int backend() const
	{
		return m_backend;
	}

private:
	net_state_ptr m_state;
	int m_backend;
};

#define debug(...) BH_LOG(m_logger, DNET_LOG_DEBUG, __VA_ARGS__)
#define notice(...) BH_LOG(m_logger, DNET_LOG_NOTICE, __VA_ARGS__)

class exec_callback
{
	public:
		typedef std::shared_ptr<exec_callback> ptr;

		exec_callback(const session &sess, const async_exec_result &result)
			: sess(sess), id(NULL), srw_data(NULL), cb(sess, result)
		{
		}

		bool start(error_info *error, complete_func func, void *priv)
		{
			cb.set_count(unlimited);

			int err = dnet_send_cmd(sess.get_native(), id, func, priv, srw_data);
			if (err < 0) {
				*error = create_error(err, "failed to execute cmd: event: %.*s, data-size: %llu",
						srw_data->event_size, srw_data->data, (unsigned long long)srw_data->data_size);
				return true;
			}

			return cb.set_count(err);
		}

		bool handle(error_info *error, struct dnet_net_state *state, struct dnet_cmd *cmd, complete_func func, void *priv)
		{
			(void) error;
			return cb.handle(state, cmd, func, priv);
		}

		void finish(const error_info &exc)
		{
			cb.complete(exc);
		}

		session sess;
		struct dnet_id *id;
		struct sph *srw_data;
		default_callback<exec_result_entry> cb;
};

template <typename T>
struct dnet_style_handler
{
	static int handler(struct dnet_net_state *state, struct dnet_cmd *cmd, void *priv)
	{
		T *callback = reinterpret_cast<T*>(priv);
		error_info error;

		if (callback->handle(&error, state, cmd, handler, priv)) {
			finish(callback, error);
		}
		return 0;
	}

	static void start(std::unique_ptr<T> &callback)
	{
		scoped_trace_id guard(callback->sess);

		error_info error;
		if (callback->start(&error, handler, callback.get())) {
			if (callback->sess.get_exceptions_policy() & session::throw_at_start)
				error.throw_error();
			// Finish is exception-safe, so it's ok to release
			// the pointer and let finish method to kill it itself
			finish(callback.release(), error);
		} else {
			// Pointer is carried by entire elliptics (it got it through cb->start call)
			// It will be killed as finished is called, which is guaranteed to be called once
			callback.release();
		}
	}

	static void finish(T *callback, const error_info &error)
	{
		callback->finish(error);
		delete callback;
	}
};

template <typename T, typename... Args>
static inline std::unique_ptr<T> createCallback(Args && ...args)
{
	return std::unique_ptr<T>(new T(args...));
}

template <typename T>
static inline void startCallback(std::unique_ptr<T> &cb)
{
	dnet_style_handler<T>::start(cb);
}

} } // namespace ioremap::elliptics

#endif // CALLBACK_P_H
