#include "elliptics/utils.hpp"
#include "elliptics/debug.hpp"

#include "session_indexes.hpp"
#include "callback_p.h"
#include "functional_p.h"

#include "../../library/elliptics.h"

namespace ioremap { namespace elliptics {

typedef async_result_handler<callback_result_entry> async_update_indexes_handler;

#undef debug
#define debug(DATA) if (1) {} else std::cerr << __PRETTY_FUNCTION__ << ":" << __LINE__ << " " << DATA << std::endl

static void on_update_index_entry(async_update_indexes_handler handler, const callback_result_entry &entry)
{
	debug("on_update_index_entry: status: " << entry.command()->status << ", size: " << entry.command()->size);

	handler.process(entry);

	if (!entry.data().empty()) {
		dnet_indexes_reply *reply = entry.data<dnet_indexes_reply>();

		for (size_t i = 0; i < reply->entries_count; ++i) {
			dnet_indexes_reply_entry &index_entry = reply->entries[i];
			dnet_addr addr = *entry.address();
			dnet_cmd cmd = *entry.command();

			memcpy(cmd.id.id, index_entry.id.id, sizeof(cmd.id.id));
			cmd.status = index_entry.status;
			cmd.size = 0;

			debug("generated: index: " << index_entry.id << ", status: " << cmd.status << ", size: " << cmd.size);

			auto data = std::make_shared<callback_result_data>(&addr, &cmd);
			handler.process(callback_result_entry(data));
		}
	}
}

static void on_update_index_finished(async_update_indexes_handler handler, const error_info &error)
{
	debug("on_update_index_finished: status: " << error.code());

	handler.complete(error);
}

// Update \a indexes for \a request_id
// Result is pushed to \a handler
async_update_indexes_result session::update_indexes(const key &request_id, const std::vector<index_entry> &indexes)
{
	transform(request_id);

	std::vector<int> groups(1, 0);

	const std::vector<int> known_groups = get_groups();

	session sess = clone();
	sess.set_filter(filters::all_with_ack);
	sess.set_checker(checkers::no_check);
	sess.set_exceptions_policy(no_exceptions);

	uint64_t data_size = 0;
	for (size_t i = 0; i < indexes.size(); ++i) {
		data_size += indexes[i].data.size();
	}

	data_buffer buffer(sizeof(dnet_indexes_request) + indexes.size() * sizeof(dnet_indexes_request_entry) + data_size);

	dnet_id indexes_id;

	dnet_indexes_request request;
	dnet_indexes_request_entry entry;
	memset(&request, 0, sizeof(request));
	memset(&entry, 0, sizeof(entry));

	request.id = request_id.id();

	dnet_indexes_transform_object_id(get_node().get_native(), &request_id.id(), &indexes_id);
	request.entries_count = indexes.size();

	buffer.write(request);

	for (size_t i = 0; i < indexes.size(); ++i) {
		const index_entry &index = indexes[i];
		entry.id = index.index;
		entry.size = index.data.size();

		buffer.write(entry);
		if (entry.size > 0) {
			buffer.write(index.data.data<char>(), index.data.size());
		}
	}

	data_pointer data(std::move(buffer));

	dnet_id &id = data.data<dnet_indexes_request>()->id;

	std::list<async_generic_result> results;

	transport_control control;
	control.set_command(DNET_CMD_INDEXES_UPDATE);
	control.set_data(data.data(), data.size());
	control.set_cflags(DNET_FLAGS_NEED_ACK);

	for (size_t i = 0; i < known_groups.size(); ++i) {
		id.group_id = known_groups[i];
		indexes_id.group_id = id.group_id;

		groups[0] = id.group_id;
		sess.set_groups(groups);

		control.set_key(indexes_id);

		async_generic_result result(sess);
		auto cb = createCallback<single_cmd_callback>(sess, result, control);

		startCallback(cb);

		results.emplace_back(std::move(result));
	}

	auto result = aggregated(sess, results.begin(), results.end());

	async_update_indexes_result final_result(*this);

	async_update_indexes_handler handler(final_result);

	result.connect(std::bind(on_update_index_entry, handler, std::placeholders::_1),
		std::bind(on_update_index_finished, handler, std::placeholders::_1));

	dnet_log(get_node().get_native(), DNET_LOG_INFO, "%s: key: %s, indexes: %zd\n",
			dnet_dump_id(&request.id), request_id.to_string().c_str(), indexes.size());

	return final_result;
}

async_update_indexes_result session::update_indexes(const key &id, const std::vector<std::string> &indexes, const std::vector<data_pointer> &datas)
{
	if (datas.size() != indexes.size())
		throw_error(-EINVAL, id, "session::update_indexes: indexes and datas sizes mismtach");

	dnet_id tmp;
	std::vector<index_entry> raw_indexes;
	raw_indexes.resize(indexes.size());

	for (size_t i = 0; i < indexes.size(); ++i) {
		transform(indexes[i], tmp);
		memcpy(raw_indexes[i].index.id, tmp.id, sizeof(tmp.id));
		raw_indexes[i].data = datas[i];
	}

	return update_indexes(id, raw_indexes);
}

typedef std::map<dnet_raw_id, dnet_raw_id, dnet_raw_id_less_than<> > dnet_raw_id_map;

struct find_all_indexes_handler
{
	session sess;
	dnet_raw_id_map map;
	async_result_handler<find_indexes_result_entry> handler;
	size_t ios_size;

	void operator() (const sync_read_result &bulk_result, const error_info &err)
	{
		std::vector<find_indexes_result_entry> result;

		if (err.code() == -ENOENT) {
			handler.complete(error_info());
			return;
		} else if (err) {
			handler.complete(err);
			return;
		}

		// If any of indexes is not found - result is empty anyway, so return now
		if (bulk_result.size() != ios_size) {
			handler.complete(error_info());
			return;
		}

		try {
			// Fill entire list by first result. All other iterations will only remove elements from it
			dnet_indexes tmp;
			indexes_unpack(sess.get_node().get_native(), &bulk_result[0].command()->id, bulk_result[0].file(), &tmp, "find_indexes_handler1");
			result.resize(tmp.indexes.size());
			for (size_t i = 0; i < tmp.indexes.size(); ++i) {
				find_indexes_result_entry &entry = result[i];
				entry.id = tmp.indexes[i].index;
				entry.indexes.push_back(std::make_pair(
					map[reinterpret_cast<dnet_raw_id&>(bulk_result[0].command()->id)],
					tmp.indexes[i].data));
			}

			for (size_t i = 1; i < bulk_result.size() && !result.empty(); ++i) {
				auto raw = reinterpret_cast<dnet_raw_id&>(bulk_result[i].command()->id);
				tmp.indexes.resize(0);
				indexes_unpack(sess.get_node().get_native(), &bulk_result[i].command()->id, bulk_result[i].file(), &tmp, "find_indexes_handler2");

				// Remove all objects from result, which are not presented for this index
				auto it = std::set_intersection(result.begin(), result.end(),
					tmp.indexes.begin(), tmp.indexes.end(),
					result.begin(),
					dnet_raw_id_less_than<skip_data>());
				result.resize(it - result.begin());

				// Remove all objects from this index, which are not presented in result
				std::set_intersection(tmp.indexes.begin(), tmp.indexes.end(),
					result.begin(), result.end(),
					tmp.indexes.begin(),
					dnet_raw_id_less_than<skip_data>());

				// As lists contain othe same objects - it's possible to add index data by one cycle
				auto jt = tmp.indexes.begin();
				for (auto kt = result.begin(); kt != result.end(); ++kt, ++jt) {
					kt->indexes.push_back(std::make_pair(map[raw], jt->data));
				}
			}
		} catch (std::exception &e) {
			handler.complete(create_error(-EINVAL, "%s", e.what()));
			return;
		}

		for (auto it = result.begin(); it != result.end(); ++it)
			handler.process(*it);
		handler.complete(error_info());
	}
};

struct find_any_indexes_handler
{
	session sess;
	dnet_raw_id_map map;
	async_result_handler<find_indexes_result_entry> handler;
	size_t ios_size;

	void operator() (const sync_read_result &bulk_result, const error_info &err)
	{
		if (err.code() == -ENOENT) {
			handler.complete(error_info());
			return;
		} else if (err) {
			handler.complete(err);
			return;
		}

		std::map<dnet_raw_id, std::vector<std::pair<dnet_raw_id, data_pointer> >, dnet_raw_id_less_than<> > result;

		try {
			dnet_indexes tmp;
			for (size_t i = 0; i < bulk_result.size(); ++i) {
				auto raw = reinterpret_cast<dnet_raw_id&>(bulk_result[i].command()->id);
				indexes_unpack(sess.get_node().get_native(), &bulk_result[i].command()->id, bulk_result[i].file(), &tmp, "find_indexes_handler3");

				for (size_t j = 0; j < tmp.indexes.size(); ++j) {
					const index_entry &entry = tmp.indexes[j];

					result[entry.index].push_back(std::make_pair(map[raw], entry.data));
				}
			}
		} catch (std::exception &e) {
			handler.complete(create_error(-EINVAL, "%s", e.what()));
			return;
		}

		for (auto it = result.begin(); it != result.end(); ++it) {
			find_indexes_result_entry entry = { it->first, it->second };
			handler.process(entry);
		}
		handler.complete(error_info());
	}
};

static std::vector<dnet_raw_id> convert(session &sess, const std::vector<std::string> &indexes)
{
	std::vector<dnet_raw_id> raw_indexes;
	raw_indexes.resize(indexes.size());

	for (size_t i = 0; i < indexes.size(); ++i) {
		sess.transform(indexes[i], raw_indexes[i]);
	}

	return std::move(raw_indexes);
}

async_find_indexes_result session::find_all_indexes(const std::vector<dnet_raw_id> &indexes)
{
	async_find_indexes_result result(*this);
	async_result_handler<find_indexes_result_entry> handler(result);

	if (indexes.size() == 0) {
		handler.complete(error_info());
		return result;
	}

	std::vector<dnet_io_attr> ios;
	struct dnet_io_attr io;
	memset(&io, 0, sizeof(io));

	dnet_raw_id_map map;

	io.flags = get_ioflags();
	dnet_raw_id index_id;
	for (size_t i = 0; i < indexes.size(); ++i) {
		index_id = transform_index_id(*this, indexes[i]);
		map[index_id] = indexes[i];
		memcpy(io.id, index_id.id, sizeof(dnet_raw_id));
		ios.push_back(io);
	}

	find_all_indexes_handler functor = { *this, map, handler, ios.size() };
	bulk_read(ios).connect(functor);

	return result;
}

async_find_indexes_result session::find_all_indexes(const std::vector<std::string> &indexes)
{
	return find_all_indexes(convert(*this, indexes));
}

async_find_indexes_result session::find_any_indexes(const std::vector<dnet_raw_id> &indexes)
{
	async_find_indexes_result result(*this);
	async_result_handler<find_indexes_result_entry> handler(result);

	if (indexes.size() == 0) {
		handler.complete(error_info());
		return result;
	}

	std::vector<dnet_io_attr> ios;
	struct dnet_io_attr io;
	memset(&io, 0, sizeof(io));

	dnet_raw_id_map map;

	io.flags = get_ioflags();
	dnet_raw_id index_id;
	for (size_t i = 0; i < indexes.size(); ++i) {
		index_id = transform_index_id(*this, indexes[i]);
		map[index_id] = indexes[i];
		memcpy(io.id, index_id.id, sizeof(dnet_raw_id));
		ios.push_back(io);
	}

	find_any_indexes_handler functor = { *this, map, handler, ios.size() };
	bulk_read(ios).connect(functor);

	return result;
}

async_find_indexes_result session::find_any_indexes(const std::vector<std::string> &indexes)
{
	return find_any_indexes(convert(*this, indexes));
}

struct check_indexes_handler
{
	session sess;
	key request_id;
	async_result_handler<index_entry> handler;

	void operator() (const sync_read_result &read_result, const error_info &err)
	{
		if (err) {
			handler.complete(err);
			return;
		}

		dnet_indexes result;
		try {
			indexes_unpack(sess.get_node().get_native(), &read_result[0].command()->id, read_result[0].file(), &result, "check_indexes_handler");
		} catch (std::exception &e) {
			handler.complete(create_error(-EINVAL, request_id, "%s", e.what()));
			return;
		}

		for (auto it = result.indexes.begin(); it != result.indexes.end(); ++it)
			handler.process(*it);
		handler.complete(error_info());
	}
};

async_check_indexes_result session::check_indexes(const key &request_id)
{
	transform(request_id);

	async_check_indexes_result result(*this);

	dnet_id id;
	memset(&id, 0, sizeof(id));
	dnet_indexes_transform_object_id(get_node().get_native(), &request_id.id(), &id);

	check_indexes_handler functor = { *this, request_id, result };
	read_latest(id, 0, 0).connect(functor);

	return result;
}

} } // ioremap::elliptics
