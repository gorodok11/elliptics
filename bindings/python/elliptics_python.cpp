/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <netdb.h>
#include <boost/python.hpp>
#include <boost/python/object.hpp>
#include <boost/python/list.hpp>
#include <boost/python/dict.hpp>
#include <boost/python/stl_iterator.hpp>

#include <elliptics/cppdef.h>

#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace bp = boost::python;

namespace ioremap { namespace elliptics {

enum elliptics_iterator_actions {
	action_start = DNET_ITERATOR_ACTION_START,
	action_pause = DNET_ITERATOR_ACTION_PAUSE,
	action_cont = DNET_ITERATOR_ACTION_CONT,
	action_cancel = DNET_ITERATOR_ACTION_CANCEL,
};

enum elliptics_iterator_types {
	itype_disk = DNET_ITYPE_DISK,
	itype_network = DNET_ITYPE_NETWORK,
};

enum elliptics_iterator_flags {
	iflag_default = 0,
	iflag_data = DNET_IFLAGS_DATA,
	iflag_key_range = DNET_IFLAGS_KEY_RANGE,
	iflag_ts_range = DNET_IFLAGS_TS_RANGE,
};

enum elliptics_cflags {
	cflags_default = 0,
	cflags_direct = DNET_FLAGS_DIRECT,
	cflags_nolock = DNET_FLAGS_NOLOCK,
};

enum elliptics_ioflags {
	ioflags_default = 0,
	ioflags_append = DNET_IO_FLAGS_APPEND,
	ioflags_compress = DNET_IO_FLAGS_COMPRESS,
	ioflags_prepare = DNET_IO_FLAGS_PREPARE,
	ioflags_commit = DNET_IO_FLAGS_COMMIT,
	ioflags_overwrite = DNET_IO_FLAGS_OVERWRITE,
	ioflags_nocsum = DNET_IO_FLAGS_NOCSUM,
	ioflags_plain_write = DNET_IO_FLAGS_PLAIN_WRITE,
	ioflags_cache = DNET_IO_FLAGS_CACHE,
	ioflags_cache_only = DNET_IO_FLAGS_CACHE_ONLY,
	ioflags_cache_remove_from_disk = DNET_IO_FLAGS_CACHE_REMOVE_FROM_DISK,
};

enum elliptics_log_level {
	log_level_data = DNET_LOG_DATA,
	log_level_error = DNET_LOG_ERROR,
	log_level_info = DNET_LOG_INFO,
	log_level_notice = DNET_LOG_NOTICE,
	log_level_debug = DNET_LOG_DEBUG,
};

static void convert_from_list(const bp::list &l, unsigned char *dst, int dlen)
{
	int length = bp::len(l);

	if (length > dlen)
		length = dlen;

	memset(dst, 0, dlen);
	for (int i = 0; i < length; ++i)
		dst[i] = bp::extract<unsigned char>(l[i]);
}

static bp::list convert_to_list(const unsigned char *src, unsigned int size)
{
	bp::list result;
	for (unsigned int i = 0; i < size; ++i)
		result.append(src[i]);
	return result;
}

struct elliptics_id {
	elliptics_id() : group_id(0) {}
	elliptics_id(bp::list id_, int group_) : id(id_), group_id(group_) {}

	elliptics_id(struct dnet_id &dnet) {
		id = convert_to_list(dnet.id, sizeof(dnet.id));
		group_id = dnet.group_id;
	}

	elliptics_id(struct dnet_raw_id &dnet) {
		id = convert_to_list(dnet.id, sizeof(dnet.id));
		group_id = 0;
	}

	struct dnet_id to_dnet() const {
		struct dnet_id dnet;
		memset(&dnet, 0, sizeof(struct dnet_id));

		convert_from_list(id, dnet.id, sizeof(dnet.id));
		dnet.group_id = group_id;

		return dnet;
	}

	bp::list	id;
	uint32_t	group_id;
};

struct elliptics_time {
	elliptics_time() : m_tsec(0), m_tnsec(0) {}
	elliptics_time(uint64_t tsec, uint64_t tnsec) : m_tsec(tsec), m_tnsec(tnsec) {}

	elliptics_time(struct dnet_time timestamp) {
		m_tsec = timestamp.tsec;
		m_tnsec = timestamp.tnsec;
	}

	dnet_time to_dnet_time() const
	{
		dnet_time ret;
		ret.tsec = m_tsec;
		ret.tnsec = m_tnsec;
		return ret;
	}

	uint64_t	m_tsec;
	uint64_t	m_tnsec;
};

struct elliptics_range {
	elliptics_range() : offset(0), size(0),
		limit_start(0), limit_num(0), ioflags(0), group_id(0) {}

	bp::list	start, end;
	uint64_t	offset, size;
	uint64_t	limit_start, limit_num;
	uint32_t	ioflags;
	int		group_id;
};

static void elliptics_extract_range(const struct elliptics_range &r, struct dnet_io_attr &io)
{
	convert_from_list(r.start, io.id, sizeof(io.id));
	convert_from_list(r.end, io.parent, sizeof(io.parent));

	io.flags = r.ioflags;
	io.size = r.size;
	io.offset = r.offset;
	io.start = r.limit_start;
	io.num = r.limit_num;
}

class elliptics_config {
	public:
		elliptics_config() {
			memset(&config, 0, sizeof(struct dnet_config));
		}

		std::string cookie_get(void) const {
			std::string ret;
			ret.assign(config.cookie, sizeof(config.cookie));
			return ret;
		}

		void cookie_set(const std::string &cookie) {
			size_t sz = sizeof(config.cookie);
			if (cookie.size() + 1 < sz)
				sz = cookie.size() + 1;
			memset(config.cookie, 0, sizeof(config.cookie));
			snprintf(config.cookie, sz, "%s", (char *)cookie.data());
		}

		struct dnet_config		config;
};

class elliptics_status : public dnet_node_status
{
	public:
		elliptics_status()
		{
			nflags = 0;
			status_flags = 0;
			log_level = 0;
		}

		elliptics_status(const dnet_node_status &other) : dnet_node_status(other)
		{
		}

		elliptics_status &operator =(const dnet_node_status &other)
		{
			dnet_node_status::operator =(other);
			return *this;
		}
};

class elliptics_node_python : public node, public bp::wrapper<node> {
	public:
		elliptics_node_python(const logger &l)
			: node(l) {}

		elliptics_node_python(const logger &l, elliptics_config &cfg)
			: node(l, cfg.config) {}

		elliptics_node_python(const node &n): node(n) {}
};

template <typename T>
static std::vector<T> convert_to_vector(const bp::api::object &list)
{
	bp::stl_input_iterator<T> begin(list), end;
	return std::vector<T>(begin, end);
}

template <typename T>
struct python_async_result
{
	typedef typename async_result<T>::iterator iterator;

	std::shared_ptr<async_result<T>> scope;

	iterator begin()
	{
		return scope->begin();
	}

	iterator end()
	{
		return scope->end();
	}

	std::vector<T> get()
	{
		return scope->get();
	}

	void wait()
	{
		scope->wait();
	}

	bool successful()
	{
		if (!scope->ready()) {
			PyErr_SetString(PyExc_ValueError, "Async write operation hasn't yet been completed");
			bp::throw_error_already_set();
		}

		return !scope->error();
	}

	bool ready()
	{
		return scope->ready();
	}

	dnet_time elapsed_time()
	{
		return scope->elapsed_time();
	}
};

template <typename T>
python_async_result<T> create_result(async_result<T> &&result)
{
	python_async_result<T> pyresult = { std::make_shared<async_result<T>>(std::move(result)) };
	return pyresult;
}

template <typename... Args>
struct def_async_result;

template <typename T>
struct def_async_result<T>
{
	static void init()
	{
		bp::class_<python_async_result<T>>("AsyncResult", bp::no_init)
			.def("__iter__", bp::iterator<python_async_result<T>>())
			.def("get", &python_async_result<T>::get)
			.def("wait", &python_async_result<T>::wait)
			.def("successful", &python_async_result<T>::successful)
			.def("ready", &python_async_result<T>::ready)
			.def("elapsed_time", &python_async_result<T>::elapsed_time)
		;
	}
};

template <>
struct def_async_result<>
{
	static void init() {}
};

template <typename T, typename... Args>
struct def_async_result<T, Args...>
{
	static void init()
	{
		def_async_result<T>::init();
		def_async_result<Args...>::init();
	}
};

typedef python_async_result<iterator_result_entry>	python_iterator_result;
typedef python_async_result<read_result_entry> 		python_read_result;
typedef python_async_result<write_result_entry>		python_write_result;

class elliptics_session: public session, public bp::wrapper<session> {
	public:
		elliptics_session(const node &n) : session(n) {}

		void set_groups(const bp::api::object &groups) {
			session::set_groups(convert_to_vector<int>(groups));
		}

		void set_direct_id(std::string saddr, int port, int family) {
			session::set_direct_id(saddr.c_str(), port, family);
		}

		struct elliptics_id get_direct_id() {
			dnet_id id = session::get_direct_id();
			return id;
		}

		bp::list get_groups() {
			std::vector<int> groups = session::get_groups();
			bp::list res;
			for(size_t i=0; i<groups.size(); i++) {
				res.append(groups[i]);
			}

			return res;
		}

		void read_file_by_id(struct elliptics_id &id, const std::string &file, uint64_t offset, uint64_t size) {
			struct dnet_id raw = id.to_dnet();
			read_file(raw, file, offset, size);
		}

		void read_file_by_data_transform(const std::string &remote, const std::string &file,
							uint64_t offset, uint64_t size) {
			read_file(key(remote), file, offset, size);
		}

		void write_file_by_id(struct elliptics_id &id, const std::string &file,
						    uint64_t local_offset, uint64_t offset, uint64_t size) {
			struct dnet_id raw = id.to_dnet();
			write_file(raw, file, local_offset, offset, size);
		}

		void write_file_by_data_transform(const std::string &remote, const std::string &file,
								uint64_t local_offset, uint64_t offset, uint64_t size) {
			write_file(key(remote), file, local_offset, offset, size);
		}

		std::string read_data_by_id(const struct elliptics_id &id, uint64_t offset, uint64_t size) {
			struct dnet_id raw = id.to_dnet();
			return read_data(raw, offset, size).get()[0].file().to_string();
		}

		std::string read_data_by_data_transform(const std::string &remote, uint64_t offset, uint64_t size) {
			return read_data(key(remote), offset, size).get()[0].file().to_string();
		}

		bp::list prepare_latest_by_id(const struct elliptics_id &id, const bp::api::object &gl) {
			struct dnet_id raw = id.to_dnet();

			std::vector<int> groups = convert_to_vector<int>(gl);

			prepare_latest(raw, groups);

			bp::list l;
			for (unsigned i = 0; i < groups.size(); ++i)
				l.append(groups[i]);

			return l;
		}

		std::string prepare_latest_by_id_str(const struct elliptics_id &id, const bp::api::object &gl) {
			struct dnet_id raw = id.to_dnet();

			std::vector<int> groups = convert_to_vector<int>(gl);

			prepare_latest(raw, groups);

			std::string ret;
			ret.assign((char *)groups.data(), groups.size() * 4);

			return ret;
		}

		std::string read_latest_by_id(const struct elliptics_id &id, uint64_t offset, uint64_t size) {
			struct dnet_id raw = id.to_dnet();
			return read_latest(raw, offset, size).get()[0].file().to_string();
		}

		std::string read_latest_by_data_transform(const std::string &remote, uint64_t offset, uint64_t size) {
			return read_latest(key(remote), offset, size).get()[0].file().to_string();
		}

		std::string convert_to_string(const sync_write_result &result)
		{
			std::string str;

			for (size_t i = 0; i < result.size(); ++i) {
				write_result_entry entry = result[i];
				str += entry.raw_data().to_string();
			}

			return str;
		}

		std::string write_data_by_id(const struct elliptics_id &id, const std::string &data, uint64_t remote_offset) {
			struct dnet_id raw = id.to_dnet();
			return convert_to_string(write_data(raw, data, remote_offset));
		}

		std::string write_data_by_data_transform(const std::string &remote, const std::string &data, uint64_t remote_offset) {
			return convert_to_string(write_data(key(remote), data, remote_offset));
		}

		python_write_result write_data_async(const bp::tuple &attr, const std::string &data) {

			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			elliptics_id& e_id = bp::extract<elliptics_id&>(attr[0]);
			dnet_id id = e_id.to_dnet();
			memcpy(io.id, id.id, sizeof(io.id));

			elliptics_time& timestamp = bp::extract<elliptics_time&>(attr[1]);
			io.timestamp = timestamp.to_dnet_time();

			io.user_flags = bp::extract<uint64_t>(attr[2]);

			return create_result(std::move(session::write_data(io, data)));
		}

		std::string write_cache_by_id(const struct elliptics_id &id, const std::string &data,
							    long timeout) {
			struct dnet_id raw = id.to_dnet();
			return convert_to_string(write_cache(raw, data, timeout));
		}

		std::string write_cache_by_data_transform(const std::string &remote, const std::string &data,
									long timeout) {
			return convert_to_string(write_cache(remote, data, timeout));
		}

		std::string lookup_addr_by_data_transform(const std::string &remote, const int group_id) {
			return lookup_address(remote, group_id);
		}

		std::string lookup_addr_by_id(const struct elliptics_id &id) {
			struct dnet_id raw = id.to_dnet();
			return lookup_address(raw, raw.group_id);
		}

		bp::tuple parse_lookup(const lookup_result_entry &lookup) {
			struct dnet_addr *addr = lookup.address();
			struct dnet_file_info *info = lookup.file_info();

			std::string address(dnet_server_convert_dnet_addr(addr));
			int port = dnet_server_convert_port((struct sockaddr *)addr->addr, addr->addr_len);

			return bp::make_tuple(address, port, info->size);
		}

		bp::tuple lookup_by_data_transform(const std::string &remote) {
			return parse_lookup(lookup(remote).get()[0]);
		}

		bp::tuple lookup_by_id(const struct elliptics_id &id) {
			struct dnet_id raw = id.to_dnet();
			return parse_lookup(lookup(raw).get()[0]);
		}

		elliptics_status update_status_by_id(const struct elliptics_id &id, elliptics_status &status) {
			struct dnet_id raw = id.to_dnet();

			update_status(raw, &status);
			return status;
		}

		elliptics_status update_status_by_string(const std::string &saddr, const int port, const int family,
								elliptics_status &status) {
			update_status(saddr.c_str(), port, family, &status);
			return status;
		}

		bp::list read_data_range(const struct elliptics_range &r) {
			struct dnet_io_attr io;
			elliptics_extract_range(r, io);

			std::vector<std::string> ret;
			ret = session::read_data_range_raw(io, r.group_id);

			bp::list l;

			for (size_t i = 0; i < ret.size(); ++i) {
				l.append(ret[i]);
			}

			return l;
		}

		bp::list get_routes() {

			std::vector<std::pair<struct dnet_id, struct dnet_addr> > routes;
			std::vector<std::pair<struct dnet_id, struct dnet_addr> >::iterator it;

			bp::list res;

			routes = session::get_routes();

			for (it = routes.begin(); it != routes.end(); it++) {
				struct elliptics_id id(it->first);
				std::string address(dnet_server_convert_dnet_addr(&(it->second)));

				res.append(bp::make_tuple(id, address));
			}

			return res;
		}

		python_iterator_result start_iterator(const elliptics_id &id, const bp::api::object &ranges,
							uint32_t type, uint64_t flags,
							const elliptics_time& time_begin = elliptics_time(0, 0),
							const elliptics_time& time_end = elliptics_time(-1, -1)) {
			std::vector<dnet_iterator_range> std_ranges = convert_to_vector<dnet_iterator_range>(ranges);
			return create_result(std::move(session::start_iterator(id.to_dnet(), std_ranges, type, flags,
							time_begin.to_dnet_time(), time_end.to_dnet_time())));
		}

		python_iterator_result pause_iterator(const elliptics_id &id, const uint64_t &iterator_id) {
			return create_result(std::move(session::pause_iterator(id.to_dnet(), iterator_id)));
		}

		python_iterator_result continue_iterator(const elliptics_id &id, const uint64_t &iterator_id) {
			return create_result(std::move(session::continue_iterator(id.to_dnet(), iterator_id)));
		}

		python_iterator_result cancel_iterator(const elliptics_id &id, const uint64_t &iterator_id) {
			return create_result(std::move(session::cancel_iterator(id.to_dnet(), iterator_id)));
		}

		std::string exec_name(const struct elliptics_id &id, const std::string &event, const std::string &data) {
			struct dnet_id raw = id.to_dnet();

			std::string result;
			sync_exec_result results = exec(&raw, event, data);
			for (size_t i = 0; i < results.size(); ++i)
				result += results[i].context().data().to_string();

			return result;

		}

		std::string exec_name_by_name(const std::string &remote, const std::string &event, const std::string &data) {
			struct dnet_id raw;
			memset(&raw, 0, sizeof(struct dnet_id));
			transform(remote, raw);

			return exec_name(raw, event, data);
		}

		void remove_by_id(const struct elliptics_id &id) {
			struct dnet_id raw = id.to_dnet();

			remove(raw).wait();
		}

		void remove_by_name(const std::string &remote) {
			remove(key(remote)).wait();
		}

		struct dnet_id_comparator
		{
			bool operator() (const struct dnet_id &first, const struct dnet_id &second) const
			{
				return memcmp(first.id, second.id, sizeof(first.id)) < 0;
			}
		};

		bp::api::object bulk_read_by_name(const bp::api::object &keys, bool raw) {
			std::vector<std::string> std_keys = convert_to_vector<std::string>(keys);

			const sync_read_result ret =  session::bulk_read(std_keys);

			if (raw) {
				bp::list result;
				for (size_t i = 0; i < ret.size(); ++i) {
					const read_result_entry entry = ret[i];
					const uint64_t size = entry.file().size();
					std::string line;
					line.append(reinterpret_cast<char*>(entry.io_attribute()->id), DNET_ID_SIZE);
					line.append(reinterpret_cast<const char*>(&size), sizeof(uint64_t));
					line.append(reinterpret_cast<char*>(entry.file().data()), size);
					result.append(line);
				}

				return result;
			} else {
				bp::dict result;

				std::map<struct dnet_id, std::string, dnet_id_comparator> keys_map;
				for (size_t i = 0; i < std_keys.size(); ++i) {
					key k(std_keys[i]);
					transform(k);
					keys_map.insert(std::make_pair(k.id(), std_keys[i]));
				}

				for (size_t i = 0; i < ret.size(); ++i) {
					const read_result_entry entry = ret[i];
					const dnet_id &id = entry.command()->id;
					result[keys_map[id]] = entry.file().to_string();
				}

				return result;
			}
		}

		bp::api::object bulk_read_by_id(const bp::api::object &keys) {
			std::vector<elliptics_id> std_keys = convert_to_vector<elliptics_id>(keys);
			std::vector<dnet_io_attr> ios;
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			ios.reserve(std_keys.size());
			for (size_t i = 0; i < std_keys.size(); ++i) {
				dnet_id id = std_keys[i].to_dnet();

				memcpy(io.id, id.id, sizeof(io.id));
				ios.push_back(io);
			}

			const sync_read_result ret =  session::bulk_read(ios);

			std::map<struct dnet_id, elliptics_id, dnet_id_comparator> keys_map;
			for (size_t i = 0; i < std_keys.size(); ++i) {
				const dnet_id id = std_keys[i].to_dnet();
				keys_map.insert(std::make_pair(id, std_keys[i]));
			}

			bp::dict result;
			for (size_t i = 0; i < ret.size(); ++i) {
				const read_result_entry entry = ret[i];
				const dnet_id &id = entry.command()->id;
				result[keys_map[id]] = entry.file().to_string();
			}

			return result;
		}

		bp::api::object bulk_read(const bp::api::object &keys)
		{
			std::vector<elliptics_id> std_keys = convert_to_vector<elliptics_id>(keys);
			std::vector<dnet_io_attr> ios;
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			ios.reserve(std_keys.size());
			std::map<struct dnet_id, elliptics_id, dnet_id_comparator> keys_map;
			for (auto it = std_keys.begin(), end = std_keys.end(); it != end; ++it) {
				dnet_id id = it->to_dnet();
				keys_map.insert(std::make_pair(id, *it));

				memcpy(io.id, id.id, sizeof(io.id));
				ios.push_back(io);
			}

			const sync_read_result res = session::bulk_read(ios);

			bp::list result;
			for (auto it = res.begin(), end = res.end(); it != end; ++it) {
				const dnet_id &id = it->command()->id;
				result.append(bp::make_tuple(keys_map[id], it->file().to_string(), it->io_attribute()->timestamp, it->io_attribute()->user_flags));
			}

			return result;
		}

		python_read_result bulk_read_async(const bp::api::object &keys)
		{
			std::vector<elliptics_id> std_keys = convert_to_vector<elliptics_id>(keys);
			std::vector<dnet_io_attr> ios;
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			ios.reserve(std_keys.size());
			std::map<struct dnet_id, elliptics_id, dnet_id_comparator> keys_map;
			for (auto it = std_keys.begin(), end = std_keys.end(); it != end; ++it) {
				dnet_id id = it->to_dnet();
				keys_map.insert(std::make_pair(id, *it));

				memcpy(io.id, id.id, sizeof(io.id));
				ios.push_back(io);
			}

			return create_result(std::move(session::bulk_read(ios)));
		}

		std::string bulk_write(const bp::api::object &data)
		{
			std::vector<bp::tuple> std_data = convert_to_vector<bp::tuple>(data);

			std::vector<dnet_io_attr> ios;
			std::vector<std::string> data_to_write;
			data_to_write.reserve(std_data.size());
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			for (auto it = std_data.begin(), end = std_data.end(); it != end; ++it) {
				elliptics_id& e_id = bp::extract<elliptics_id&>((*it)[0]);
				dnet_id id = e_id.to_dnet();

				std::string data = bp::extract<std::string>((*it)[1]);

				io.timestamp = bp::extract<dnet_time>((*it)[2]);
				io.user_flags = bp::extract<uint64_t>((*it)[3]);

				memcpy(io.id, id.id, sizeof(io.id));
				io.size = data.size();
				data_to_write.push_back(data);

				ios.push_back(io);
			}

			return convert_to_string(session::bulk_write(ios, data_to_write));
		}

		std::string bulk_write_by_id(const bp::api::object &keys, const bp::api::object &data) {
			std::vector<elliptics_id> std_keys = convert_to_vector<elliptics_id>(keys);
			std::vector<std::string> std_data = convert_to_vector<std::string>(data);

			std::vector<dnet_io_attr> ios;
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			for (size_t i = 0; i < std_keys.size(); ++i) {
				dnet_id id = std_keys[i].to_dnet();

				memcpy(io.id, id.id, sizeof(io.id));
				io.size = std_data[i].size();

				ios.push_back(io);
			}

			return convert_to_string(session::bulk_write(ios, std_data));
		}

		std::string bulk_write_by_name(const bp::api::object &keys, const bp::api::object &data) {
			std::vector<std::string> std_keys = convert_to_vector<std::string>(keys);
			std::vector<std::string> std_data = convert_to_vector<std::string>(data);

			std::vector<dnet_io_attr> ios;
			dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			for (size_t i = 0; i < std_keys.size(); ++i) {
				key id = std_keys[i];
				transform(id);

				memcpy(io.id, id.id().id, sizeof(io.id));
				io.size = std_data[i].size();

				ios.push_back(io);
			}

			return convert_to_string(session::bulk_write(ios, std_data));
		}

		bp::list stat_log_count() {
			bp::list statistics;

			const sync_stat_count_result result = session::stat_log_count();

			for (size_t i = 0; i < result.size(); ++i) {
				const stat_count_result_entry &data = result[i];

				if (data.size() <= sizeof(struct dnet_addr_stat))
					continue;

				bp::dict node_stat, storage_commands, proxy_commands, counters;
				struct dnet_addr *addr = data.address();
				struct dnet_cmd *cmd = data.command();

				struct dnet_addr_stat *as = data.statistics();

				std::string address(dnet_server_convert_dnet_addr(addr));
				node_stat[std::string("addr")] = address;
				node_stat[std::string("group_id")] = cmd->id.group_id;

				for (int j = 0; j < as->num; ++j) {
					if (j < as->cmd_num) {
						storage_commands[std::string(dnet_counter_string(j, as->cmd_num))] =
								bp::make_tuple((unsigned long long)as->count[j].count,
										(unsigned long long)as->count[j].err);
					} else if (j < (as->cmd_num * 2)) {
						proxy_commands[std::string(dnet_counter_string(j, as->cmd_num))] =
								bp::make_tuple((unsigned long long)as->count[j].count,
										(unsigned long long)as->count[j].err);
					} else {
						counters[std::string(dnet_counter_string(j, as->cmd_num))] =
								bp::make_tuple((unsigned long long)as->count[j].count,
										(unsigned long long)as->count[j].err);
					}
				}

				node_stat["storage_commands"] = storage_commands;
				node_stat["proxy_commands"] = proxy_commands;
				node_stat["counters"] = counters;

				statistics.append(node_stat);
			}

			return statistics;
		}
};

class elliptics_error_translator
{
	public:
		elliptics_error_translator()
		{
		}

		void operator() (const error &err) const
		{
			bp::api::object exception(err);
			bp::api::object type = m_type;
			for (size_t i = 0; i < m_types.size(); ++i) {
				if (m_types[i].first == err.error_code()) {
					type = m_types[i].second;
					break;
				}
			}
			PyErr_SetObject(type.ptr(), exception.ptr());
		}

		void initialize()
		{
			m_type = new_exception("Error");
			register_type(-ENOENT, "NotFoundError");
			register_type(-ETIMEDOUT, "TimeoutError");
		}

		void register_type(int code, const char *name)
		{
			register_type(code, new_exception(name, m_type.ptr()));
		}

		void register_type(int code, const bp::api::object &type)
		{
			m_types.push_back(std::make_pair(code, type));
		}

	private:
		bp::api::object new_exception(const char *name, PyObject *parent = NULL)
		{
			std::string scopeName = bp::extract<std::string>(bp::scope().attr("__name__"));
			std::string qualifiedName = scopeName + "." + name;

			PyObject *type = PyErr_NewException(&qualifiedName[0], parent, 0);
			if (!type)
				bp::throw_error_already_set();
			bp::api::object type_object = bp::api::object(bp::handle<>(type));
			bp::scope().attr(name) = type_object;
			return type_object;
		}

		bp::api::object m_type;
		std::vector<std::pair<int, bp::api::object> > m_types;
};

void ios_base_failure_translator(const std::ios_base::failure &exc)
{
	PyErr_SetString(PyExc_IOError, exc.what());
}

BOOST_PYTHON_MEMBER_FUNCTION_OVERLOADS(add_remote_overloads, add_remote, 2, 3);

std::string dnet_node_status_repr(const dnet_node_status &status)
{
	char buffer[128];
	const size_t buffer_size = sizeof(buffer);
	snprintf(buffer, buffer_size,
		"<SessionStatus nflags:%x, status_flags:%x, log_mask:%x>",
		status.nflags, status.status_flags, status.log_level);
	buffer[buffer_size - 1] = '\0';
	return buffer;
}

void logger_log(logger &log, const char *msg, int level)
{
	log.log(level, msg);
}

void next_impl(bp::api::object &value, const bp::api::object &next)
{
	value = next();
}

bp::list dnet_iterator_range_get_key_begin(const dnet_iterator_range *range)
{
	return convert_to_list(range->key_begin.id, sizeof(range->key_begin.id));
}

void dnet_iterator_range_set_key_begin(dnet_iterator_range *range, const bp::list &list)
{
	convert_from_list(list, range->key_begin.id, sizeof(range->key_begin.id));
}

bp::list dnet_iterator_range_get_key_end(const dnet_iterator_range *range)
{
	return convert_to_list(range->key_end.id, sizeof(range->key_end.id));
}

void dnet_iterator_range_set_key_end(dnet_iterator_range *range, const bp::list &list)
{
	convert_from_list(list, range->key_end.id, sizeof(range->key_end.id));
}

dnet_iterator_response iterator_result_response(iterator_result_entry result)
{
	return *result.reply();
}

std::string iterator_result_response_data(iterator_result_entry result)
{
	return result.reply_data().to_string();
}

bp::list iterator_response_get_key(dnet_iterator_response *response)
{
	return convert_to_list(response->key.id, sizeof(response->key.id));
}

elliptics_time iterator_response_get_timestamp(dnet_iterator_response *response)
{
	return elliptics_time(response->timestamp);
}

uint64_t iterator_response_get_user_flags(dnet_iterator_response *response)
{
	return response->user_flags;
}

void iterator_container_append_rr(iterator_result_container &container,
		dnet_iterator_response &response)
{
	container.append(&response);
}

void iterator_container_append(iterator_result_container &container,
		iterator_result_entry &result)
{
	container.append(result);
}

void iterator_container_sort(iterator_result_container &container)
{
	container.sort();
}

uint64_t iterator_container_get_count(const iterator_result_container &container)
{
	return container.m_count;
}

dnet_iterator_response iterator_container_getitem(const iterator_result_container &container,
		uint64_t n)
{
	if (n >= container.m_count) {
		PyErr_SetString(PyExc_IndexError, "Index out of range");
		bp::throw_error_already_set();
	}
	return container[n];
}

void iterator_container_diff(iterator_result_container &left,
		iterator_result_container &right, iterator_result_container &diff)
{
	left.diff(right, diff);
}

void iterator_container_merge(const bp::list& /*results*/, bp::dict& /*splitted_dict*/)
{
}

std::string read_result_get_data(read_result_entry &result)
{
	return result.file().to_string();
}

elliptics_id read_result_get_id(read_result_entry &result)
{
	return elliptics_id(convert_to_list(result.io_attribute()->id, sizeof(result.io_attribute()->id)), 0);
}

elliptics_time read_result_get_timestamp(read_result_entry &result)
{
	return elliptics_time(result.io_attribute()->timestamp);
}

uint64_t read_result_get_user_flags(read_result_entry &result)
{
	return result.io_attribute()->user_flags;
}

struct id_pickle : bp::pickle_suite
{
	static bp::tuple getinitargs(const elliptics_id& id)
	{
		return getstate(id);
	}

	static bp::tuple getstate(const elliptics_id& id)
	{
		return bp::make_tuple(id.id, id.group_id);
	}

	static void setstate(elliptics_id& id, bp::tuple state)
	{
		if (len(state) != 2)
		{
			PyErr_SetObject(PyExc_ValueError,
				("expected 2-item tuple in call to __setstate__; got %s"
					% state).ptr()
				);
			bp::throw_error_already_set();
		}

		id.id = bp::extract<bp::list>(state[0]);
		id.group_id = bp::extract<uint32_t>(state[1]);
	}
};

struct time_pickle : bp::pickle_suite
{
	static bp::tuple getinitargs(const elliptics_time& time)
	{
		return getstate(time);
	}

	static bp::tuple getstate(const elliptics_time& time)
	{
		return bp::make_tuple(time.m_tsec, time.m_tnsec);
	}

	static void setstate(elliptics_time& time, bp::tuple state)
	{
		if (len(state) != 2)
		{
			PyErr_SetObject(PyExc_ValueError,
				("expected 2-item tuple in call to __setstate__; got %s"
					% state).ptr()
				);
			bp::throw_error_already_set();
		}

		time.m_tsec = bp::extract<uint64_t>(state[0]);
		time.m_tnsec = bp::extract<uint64_t>(state[1]);
	}
};

BOOST_PYTHON_MODULE(elliptics) {
	bp::class_<error> error_class("ErrorInfo", bp::init<int, std::string>());
	error_class.def("__str__", &error::error_message);
	error_class.add_property("message", &error::error_message);
	error_class.add_property("code", &error::error_code);
	elliptics_error_translator error_translator;
	error_translator.initialize();


	bp::register_exception_translator<timeout_error>(error_translator);
	bp::register_exception_translator<not_found_error>(error_translator);
	bp::register_exception_translator<error>(error_translator);
	bp::register_exception_translator<std::ios_base::failure>(ios_base_failure_translator);

	bp::class_<elliptics_id>("Id")
		.def(bp::init<bp::list, int>(bp::args("key", "group_id")))
		.def_readwrite("id", &elliptics_id::id)
		.def_readwrite("group_id", &elliptics_id::group_id)
		.def_pickle(id_pickle())
	;

	bp::class_<elliptics_time>("Time",
			bp::init<uint64_t, uint64_t>(bp::args("tsec", "tnsec")))
		.def_readwrite("tsec", &elliptics_time::m_tsec)
		.def_readwrite("tnsec", &elliptics_time::m_tnsec)
		.def_pickle(time_pickle())
	;

	bp::class_<dnet_iterator_range>("IteratorRange")
		.add_property("key_begin", dnet_iterator_range_get_key_begin,
				dnet_iterator_range_set_key_begin)
		.add_property("key_end", dnet_iterator_range_get_key_end,
				dnet_iterator_range_set_key_end)
	;

	def_async_result<	callback_result_entry,
						write_result_entry,
						lookup_result_entry,
						read_result_entry,
						stat_result_entry,
						stat_count_result_entry,
						iterator_result_entry,
						exec_result_entry,
						find_indexes_result_entry,
						index_entry
					>::init();

	bp::class_<iterator_result_entry>("IteratorResultEntry")
		.add_property("id", &iterator_result_entry::id)
		.add_property("status", &iterator_result_entry::status)
		.add_property("response", iterator_result_response)
		.add_property("response_data", iterator_result_response_data)
	;

	bp::class_<dnet_iterator_response>("IteratorResultResponse",
			bp::no_init)
		.add_property("key", iterator_response_get_key)
		.add_property("timestamp", iterator_response_get_timestamp)
		.add_property("user_flags", iterator_response_get_user_flags)
	;

	bp::class_<iterator_result_container>("IteratorResultContainer",
			bp::init<int>(bp::args("fd")))
		.add_property("fd", &iterator_result_container::m_fd)
		.def(bp::init<int, bool, uint64_t>(bp::args("fd", "sorted", "write_position")))
		.def("append", iterator_container_append)
		.def("append_rr", iterator_container_append_rr)
		.def("sort", iterator_container_sort)
		.def("diff", iterator_container_diff)
		.def("__len__", iterator_container_get_count)
		.def("__getitem__", iterator_container_getitem)
		.def("merge", &iterator_container_merge)
		.staticmethod("merge")
	;

	bp::class_<read_result_entry>("ReadResultEntry")
		.add_property("data", read_result_get_data)
		.add_property("id", read_result_get_id)
		.add_property("timestamp", read_result_get_timestamp)
		.add_property("user_flags", read_result_get_user_flags)
	;

	bp::class_<write_result_entry>("WriteResultEntry")
	;

	bp::class_<elliptics_range>("Range")
		.def_readwrite("start", &elliptics_range::start)
		.def_readwrite("end", &elliptics_range::end)
		.def_readwrite("offset", &elliptics_range::offset)
		.def_readwrite("size", &elliptics_range::size)
		.def_readwrite("ioflags", &elliptics_range::ioflags)
		.def_readwrite("group_id", &elliptics_range::group_id)
		.def_readwrite("limit_start", &elliptics_range::limit_start)
		.def_readwrite("limit_num", &elliptics_range::limit_num)
	;

	bp::class_<logger, boost::noncopyable>("AbstractLogger", bp::no_init)
		.def("log", &logger::log)
	;

	bp::class_<file_logger, bp::bases<logger> > file_logger_class(
		"Logger", bp::init<const char *, const uint32_t>());

	bp::class_<elliptics_status>("SessionStatus", bp::init<>())
		.def_readwrite("nflags", &dnet_node_status::nflags)
		.def_readwrite("status_flags", &dnet_node_status::status_flags)
		.def_readwrite("log_level", &dnet_node_status::log_level)
		.def("__repr__", dnet_node_status_repr)
	;

	bp::class_<dnet_config>("dnet_config", bp::no_init)
		.def_readwrite("wait_timeout", &dnet_config::wait_timeout)
		.def_readwrite("flags", &dnet_config::flags)
		.def_readwrite("check_timeout", &dnet_config::check_timeout)
		.def_readwrite("io_thread_num", &dnet_config::io_thread_num)
		.def_readwrite("nonblocking_io_thread_num", &dnet_config::nonblocking_io_thread_num)
		.def_readwrite("net_thread_num", &dnet_config::net_thread_num)
		.def_readwrite("client_prio", &dnet_config::client_prio)
	;

	bp::class_<dnet_time>("dnet_time", bp::no_init)
		.def_readwrite("tsec", &dnet_time::tsec)
		.def_readwrite("tnsec", &dnet_time::tnsec)
	;

	bp::class_<elliptics_config>("Config", bp::init<>())
		.def_readwrite("config", &elliptics_config::config)
		.add_property("cookie", &elliptics_config::cookie_get, &elliptics_config::cookie_set)
	;

	bp::class_<elliptics_node_python>("Node", bp::init<logger>())
		.def(bp::init<logger, elliptics_config &>())
		.def("add_remote", static_cast<void (node::*)(const char*, int, int)>(&node::add_remote),
			(bp::arg("addr"), bp::arg("port"), bp::arg("family") = AF_INET))
	;

	bp::class_<elliptics_session, boost::noncopyable>("Session", bp::init<node &>())
		.add_property("groups", &elliptics_session::get_groups, &elliptics_session::set_groups)
		.def("add_groups", &elliptics_session::set_groups)
		.def("set_groups", &elliptics_session::set_groups)
		.def("get_groups", &elliptics_session::get_groups)

		.add_property("cflags", &elliptics_session::get_cflags, &elliptics_session::set_cflags)
		.def("set_cflags", &elliptics_session::set_cflags)
		.def("get_cflags", &elliptics_session::get_cflags)

		.add_property("ioflags", &elliptics_session::get_ioflags, &elliptics_session::set_ioflags)
		.def("set_ioflags", &elliptics_session::set_ioflags)
		.def("get_ioflags", &elliptics_session::get_ioflags)

		.def("set_direct_id", &elliptics_session::set_direct_id)
		.def("get_direct_id", &elliptics_session::get_direct_id)

		.def("read_file", &elliptics_session::read_file_by_id,
			(bp::arg("key"), bp::arg("filename"), bp::arg("offset") = 0, bp::arg("size") = 0))
		.def("read_file", &elliptics_session::read_file_by_data_transform,
			(bp::arg("key"), bp::arg("filename"), bp::arg("offset") = 0, bp::arg("size") = 0))
		.def("write_file", &elliptics_session::write_file_by_id,
			(bp::arg("key"), bp::arg("filename"), bp::arg("offset") = 0, bp::arg("local_offset") = 0, bp::arg("size") = 0))
		.def("write_file", &elliptics_session::write_file_by_data_transform,
			(bp::arg("key"), bp::arg("filename"), bp::arg("offset") = 0, bp::arg("local_offset") = 0, bp::arg("size") = 0))

		.def("read_data", &elliptics_session::read_data_by_id,
			(bp::arg("key"), bp::arg("offset") = 0, bp::arg("size") = 0))
		.def("read_data", &elliptics_session::read_data_by_data_transform,
			(bp::arg("key"), bp::arg("offset") = 0, bp::arg("size") = 0))

		.def("prepare_latest", &elliptics_session::prepare_latest_by_id)
		.def("prepare_latest_str", &elliptics_session::prepare_latest_by_id_str)

		.def("read_latest", &elliptics_session::read_latest_by_id,
			(bp::arg("key"), bp::arg("offset") = 0, bp::arg("size") = 0))
		.def("read_latest", &elliptics_session::read_latest_by_data_transform,
			(bp::arg("key"), bp::arg("offset") = 0, bp::arg("size") = 0, bp::arg("column") = 0))

		.def("write_data", &elliptics_session::write_data_by_id,
			(bp::arg("key"), bp::arg("data"), bp::arg("offset") = 0))
		.def("write_data", &elliptics_session::write_data_by_data_transform,
			(bp::arg("key"), bp::arg("data"), bp::arg("offset") = 0, bp::arg("column") = 0))
		.def("write_data_async", &elliptics_session::write_data_async,
			(bp::arg("io_attr"), bp::arg("data")))

		.def("write_cache", &elliptics_session::write_cache_by_id)
		.def("write_cache", &elliptics_session::write_cache_by_data_transform)

		.def("lookup_addr", &elliptics_session::lookup_addr_by_data_transform)
		.def("lookup_addr", &elliptics_session::lookup_addr_by_id)

		.def("lookup", &elliptics_session::lookup_by_data_transform)
		.def("lookup", &elliptics_session::lookup_by_id)

		.def("update_status", &elliptics_session::update_status_by_id)
		.def("update_status", &elliptics_session::update_status_by_string)

		.def("read_data_range", &elliptics_session::read_data_range)

		.def("get_routes", &elliptics_session::get_routes)
		.def("stat_log", &elliptics_session::stat_log_count)

		.def("start_iterator", &elliptics_session::start_iterator)
		.def("pause_iterator", &elliptics_session::pause_iterator)
		.def("continue_iterator", &elliptics_session::continue_iterator)
		.def("cancel_iterator", &elliptics_session::cancel_iterator)

		.def("exec_event", &elliptics_session::exec_name)
		.def("exec_event", &elliptics_session::exec_name_by_name)

		.def("remove", &elliptics_session::remove_by_id)
		.def("remove", &elliptics_session::remove_by_name)

		.def("bulk_read", &elliptics_session::bulk_read,
			(bp::arg("keys")))
		.def("bulk_read_async", &elliptics_session::bulk_read_async,
			(bp::arg("keys")))
		.def("bulk_read_by_name", &elliptics_session::bulk_read_by_name,
			(bp::arg("keys"), bp::arg("raw") = false))
		.def("bulk_read_by_id", &elliptics_session::bulk_read_by_id,
			(bp::arg("keys")))

		.def("bulk_write", &elliptics_session::bulk_write,
			(bp::arg("datas")))
		.def("bulk_write_by_id", &elliptics_session::bulk_write_by_id,
			(bp::arg("keys"), bp::arg("data")))
		.def("bulk_write_by_name", &elliptics_session::bulk_write_by_name,
			(bp::arg("keys"), bp::arg("data")))
	;

	bp::enum_<elliptics_iterator_actions>("iterator_actions")
		.value("start", action_start)
		.value("pause", action_pause)
		.value("cont", action_cont)
		.value("cancel", action_cancel)
	;

	bp::enum_<elliptics_iterator_flags>("iterator_flags")
		.value("default", iflag_default)
		.value("data", iflag_data)
		.value("key_range", iflag_key_range)
		.value("ts_range", iflag_ts_range)
	;

	bp::enum_<elliptics_iterator_types>("iterator_types")
		.value("disk", itype_disk)
		.value("network", itype_network)
	;

	bp::enum_<elliptics_cflags>("command_flags")
		.value("default", cflags_default)
		.value("direct", cflags_direct)
		.value("nolock", cflags_nolock)
	;

	bp::enum_<elliptics_ioflags>("io_flags")
		.value("default", ioflags_default)
		.value("append", ioflags_append)
		.value("compress", ioflags_compress)
		.value("prepare", ioflags_prepare)
		.value("commit", ioflags_commit)
		.value("overwrite", ioflags_overwrite)
		.value("nocsum", ioflags_nocsum)
		.value("plain_write", ioflags_plain_write)
		.value("nodata", ioflags_plain_write)
		.value("cache", ioflags_cache)
		.value("cache_only", ioflags_cache_only)
		.value("cache_remove_from_disk", ioflags_cache_remove_from_disk)
	;

	bp::enum_<elliptics_log_level>("log_level")
		.value("data", log_level_data)
		.value("error", log_level_error)
		.value("info", log_level_info)
		.value("notice", log_level_notice)
		.value("debug", log_level_debug)
	;
};

} } // namespace ioremap::elliptics
