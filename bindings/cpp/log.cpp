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

#include <elliptics/cppdef.h>

using namespace ioremap::elliptics;

static void	real_logger(void *priv, const int level, const char *msg)
{
	logger *log = reinterpret_cast<logger *> (priv);

	log->log(level, msg);
}

class ioremap::elliptics::logger_data {
	public:
		logger_data(logger *that, logger_interface *interface, int level) : impl(interface) {
			log.log_level = level;
			log.log = real_logger;
			log.log_private = that;
		}
		~logger_data() {
			delete impl;
		}

		dnet_log log;
		logger_interface *impl;
};

logger::logger(logger_interface *interface, const int level)
	: m_data(new logger_data(this, interface, level)) {
}

logger::logger() : m_data(new logger_data(this, NULL, DNET_LOG_INFO)) {
}

logger::logger(const logger &other) : m_data(other.m_data) {
}

logger::~logger() {
}

logger &logger::operator =(const logger &other) {
	m_data = other.m_data;
	return *this;
}

void logger::log(const int level, const char *msg)
{
	if (level <= m_data->log.log_level && m_data->impl) {
		m_data->impl->log(level, msg);
	}
}

int logger::get_log_level()
{
	return m_data->log.log_level;
}

dnet_log *logger::get_dnet_log()
{
	return &m_data->log;
}

class file_logger_interface : public logger_interface {
	public:
		file_logger_interface(const char *file) {
			m_stream.open(file, std::ios_base::app);
			if (!m_stream) {
				std::string message = "Can not open file: \"";
				message += file;
				message += "\"";
				throw std::ios_base::failure(message);
			}
			m_stream.exceptions(std::ofstream::failbit);
		}
		~file_logger_interface() {
		}

		void log(int level, const char *msg)
		{
			(void) level;
			char str[64];
			struct tm tm;
			struct timeval tv;
			char usecs_and_id[64];

			gettimeofday(&tv, NULL);
			localtime_r((time_t *)&tv.tv_sec, &tm);
			strftime(str, sizeof(str), "%F %R:%S", &tm);

			snprintf(usecs_and_id, sizeof(usecs_and_id), ".%06lu %ld/%d : ", tv.tv_usec, dnet_get_id(), getpid());

			m_stream << str << usecs_and_id << msg;
			m_stream.flush();
		}

	private:
		std::ofstream	m_stream;
};

file_logger::file_logger(const char *file, const int level) :
	logger(new file_logger_interface(file), level)
{
}

file_logger::~file_logger()
{
}
