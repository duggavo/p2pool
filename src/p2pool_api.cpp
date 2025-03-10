/*
 * This file is part of the Monero P2Pool <https://github.com/SChernykh/p2pool>
 * Copyright (c) 2021-2023 SChernykh <https://github.com/SChernykh>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "p2pool_api.h"

#ifdef _MSC_VER
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static constexpr char log_category_prefix[] = "P2Pool API ";

namespace p2pool {

p2pool_api::p2pool_api(const std::string& api_path, const bool local_stats)
	: m_apiPath(api_path)
	, m_counter(0)
{
	if (m_apiPath.empty()) {
		LOGERR(1, "api path is empty");
		panic();
	}

	if ((m_apiPath.back() != '/')
#ifdef _WIN32
		&& (m_apiPath.back() != '\\')
#endif
		) {
		m_apiPath += '/';
	}

	struct stat buf;
	if (stat(m_apiPath.c_str(), &buf) != 0) {
		LOGERR(1, "path " << m_apiPath << " doesn't exist");
		panic();
	}

	int result = uv_async_init(uv_default_loop_checked(), &m_dumpToFileAsync, on_dump_to_file);
	if (result) {
		LOGERR(1, "uv_async_init failed, error " << uv_err_name(result));
		panic();
	}
	m_dumpToFileAsync.data = this;

	uv_mutex_init_checked(&m_dumpDataLock);

	m_networkPath = m_apiPath + "network/";
	m_poolPath = m_apiPath + "pool/";
	m_localPath = m_apiPath + "local/";

	create_dir(m_networkPath);
	create_dir(m_poolPath);

	if (local_stats) {
		create_dir(m_localPath);
	}
}

p2pool_api::~p2pool_api()
{
	uv_mutex_destroy(&m_dumpDataLock);
}

void p2pool_api::create_dir(const std::string& path)
{
#ifdef _MSC_VER
	int result = _mkdir(path.c_str());
#else
	int result = mkdir(path.c_str()
#ifndef _WIN32
		, 0775
#endif
	);
#endif

	if (result < 0) {
		result = errno;
		if (result != EEXIST) {
			LOGERR(1, "mkdir(" << path << ") failed, error " << result);
			panic();
		}
	}
}

void p2pool_api::on_stop()
{
	uv_close(reinterpret_cast<uv_handle_t*>(&m_dumpToFileAsync), nullptr);
}

void p2pool_api::dump_to_file_async_internal(Category category, const char* filename, DumpFileCallbackBase&& callback)
{
	std::vector<char> buf(16384);
	log::Stream s(buf.data(), buf.size());
	callback(s);
	buf.resize(s.m_pos);

	std::string path;

	switch (category) {
	case Category::GLOBAL:  path = m_apiPath     + filename; break;
	case Category::NETWORK: path = m_networkPath + filename; break;
	case Category::POOL:    path = m_poolPath    + filename; break;
	case Category::LOCAL:   path = m_localPath   + filename; break;
	}

	{
		MutexLock lock(m_dumpDataLock);
		m_dumpData[path] = std::move(buf);
	}

	uv_async_send(&m_dumpToFileAsync);
}

void p2pool_api::dump_to_file()
{
	unordered_map<std::string, std::vector<char>> data;
	{
		MutexLock lock(m_dumpDataLock);
		data = std::move(m_dumpData);
	}

	for (auto& it : data) {
		DumpFileWork* work = new DumpFileWork{ {}, 0, it.first, it.first + std::to_string(m_counter), std::move(it.second) };
		work->req.data = work;
		++m_counter;

		const int flags = O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_BINARY
			| O_BINARY
#endif
			;

		const int result = uv_fs_open(uv_default_loop_checked(), &work->req, work->tmp_name.c_str(), flags, 0644, on_fs_open);
		if (result < 0) {
			LOGWARN(4, "failed to open " << work->tmp_name << ", error " << uv_err_name(result));
			delete work;
		}
	}
}

void p2pool_api::on_fs_open(uv_fs_t* req)
{
	DumpFileWork* work = reinterpret_cast<DumpFileWork*>(req->data);
	work->fd = static_cast<int>(req->result);
	uv_fs_req_cleanup(req);

	if (work->fd < 0) {
		LOGWARN(4, "failed to open " << work->tmp_name << ", error " << uv_err_name(work->fd));
		delete work;
		return;
	}

	uv_buf_t buf[1];
	buf[0].base = work->buf.data();
	buf[0].len = static_cast<uint32_t>(work->buf.size());

	const int result = uv_fs_write(uv_default_loop_checked(), &work->req, static_cast<uv_file>(work->fd), buf, 1, 0, on_fs_write);
	if (result < 0) {
		LOGWARN(4, "failed to write to " << work->tmp_name << ", error " << uv_err_name(result));
		delete work;
		return;
	}
}

void p2pool_api::on_fs_write(uv_fs_t* req)
{
	DumpFileWork* work = reinterpret_cast<DumpFileWork*>(req->data);

	if (req->result < 0) {
		LOGWARN(4, "failed to write to " << work->tmp_name << ", error " << uv_err_name(static_cast<int>(req->result)));
	}

	uv_fs_req_cleanup(req);

	const int result = uv_fs_close(uv_default_loop_checked(), &work->req, static_cast<uv_file>(work->fd), on_fs_close);
	if (result < 0) {
		LOGWARN(4, "failed to close " << work->tmp_name << ", error " << uv_err_name(result));
		delete work;
		return;
	}
}

void p2pool_api::on_fs_close(uv_fs_t* req)
{
	DumpFileWork* work = reinterpret_cast<DumpFileWork*>(req->data);

	if (req->result < 0) {
		LOGWARN(4, "failed to close " << work->tmp_name << ", error " << uv_err_name(static_cast<int>(req->result)));
	}

	uv_fs_req_cleanup(req);

	const int result = uv_fs_rename(uv_default_loop_checked(), &work->req, work->tmp_name.c_str(), work->name.c_str(), on_fs_rename);
	if (result < 0) {
		LOGWARN(4, "failed to rename " << work->tmp_name << " to " << work->name << ", error " << uv_err_name(result));
		delete work;
		return;
	}
}

void p2pool_api::on_fs_rename(uv_fs_t* req)
{
	DumpFileWork* work = reinterpret_cast<DumpFileWork*>(req->data);

	if (req->result < 0) {
		LOGWARN(4, "failed to rename " << work->tmp_name << " to " << work->name << ", error " << uv_err_name(static_cast<int>(req->result)));
	}

	uv_fs_req_cleanup(req);
	delete work;
}

} // namespace p2pool
