
#include "condor_common.h"
#include "string_list.h"
#include "condor_attributes.h"
#include "ipv6_hostname.h"
#include "condor_daemon_core.h"
#include "condor_config.h"

#include "stdsoap2.h"
#include <sstream>

#include "transfer_input.h"

extern DaemonCore *daemonCore;

TransferInputHttp* TransferInputHttp::g_input = NULL;

int
TransferInputHttp::GetHandler(struct soap * gsoap) {
	TransferInputHttp & input = TransferInputHttp::GetInstance();
	return input.m_condor_get_handler(gsoap);
}

TransferInputHttp & TransferInputHttp::GetInstance()
{
	if (g_input == NULL) {
		g_input = new TransferInputHttp();
	}
	return *g_input;
}

void
TransferInputHttp::Reconfig()
{
	if (daemonCore == NULL)
		return;
	DaemonCore &dc = *daemonCore;

	struct soap * gsoap = dc.GetSoap();
	m_condor_get_handler = gsoap->fget;
	gsoap->fget = GetHandler;

	m_transfer_forker.Initialize();
	int max_transfer_forkers = param_integer("SCHEDD_HTTP_TRANSFER_WORKERS",3,0);
	m_transfer_forker.setMaxWorkers(max_transfer_forkers);
}

bool
TransferInputHttp::SpoolReady(compat_classad::ClassAd& jobad)
{
	std::string transfer_input_files;
	bool has_input_files = jobad.LookupString(ATTR_TRANSFER_INPUT_FILES, transfer_input_files);
	StringList input_files_list(has_input_files ? transfer_input_files.c_str() : "", ",");

	std::string cache_input_files;
	bool has_cache_files = jobad.LookupString(ATTR_TRANSFER_CACHE_INPUT_FILES, cache_input_files);
	if (!has_cache_files)
		return false;
	StringList cache_files_list(cache_input_files.c_str(), ",");

	std::string dont_encrypt_files;
	bool has_dont_encrypt = jobad.LookupString(ATTR_ENCRYPT_INPUT_FILES, dont_encrypt_files);
	StringList dont_encrypt_files_list(has_dont_encrypt ? dont_encrypt_files.c_str() : "", ",");

	std::string encrypt_files;
	bool has_encrypt = jobad.LookupString(ATTR_ENCRYPT_INPUT_FILES, encrypt_files);
	StringList encrypt_files_list(has_encrypt ? encrypt_files.c_str() : "", ",");

	std::vector<std::string> resulting_input_files;
	std::vector<std::string> resulting_cache_files;
	input_files_list.rewind();
	const char * next_file;
	while ( (next_file = input_files_list.next()) ) {
		if (cache_files_list.contains_withwildcard(next_file) &&
				(!encrypt_files_list.contains_withwildcard(next_file) ||
				 dont_encrypt_files_list.contains_withwildcard(next_file))) {
			resulting_cache_files.push_back(next_file);
		} else {
			resulting_input_files.push_back(next_file);
		}
	}

	std::string user;
	if (!jobad.LookupString(ATTR_OWNER, user)) {
		dprintf(D_ALWAYS, "No username associated with job.\n");
		return false;
	}
	for (std::vector<std::string>::const_iterator it=resulting_cache_files.begin(); it != resulting_cache_files.end(); it++)
	{
		dprintf(D_FULLDEBUG, "Attempting to cache input file %s.\n", it->c_str());
		std::string resulting_name;
		if (AddFile(*it, user, resulting_name)) {
			resulting_cache_files.push_back(resulting_name);
		} else {
			resulting_input_files.push_back(*it);
		}
	}

	if (!resulting_input_files.empty()) {
		std::string resulting_files;
		join(resulting_input_files, ",", resulting_files);
		jobad.Assign(ATTR_TRANSFER_INPUT_FILES, resulting_files);
		jobad.SetDirtyFlag(ATTR_TRANSFER_INPUT_FILES, true);
		join(resulting_cache_files, ",", resulting_files);
		jobad.Assign(ATTR_TRANSFER_CACHE_INPUT_FILES, resulting_files);
		jobad.SetDirtyFlag(ATTR_TRANSFER_CACHE_INPUT_FILES, true);
		return true;
	} else {
		return false;
	}
}

bool
TransferInputHttp::AddFile(const std::string &file, const std::string &user, std::string &resulting_name)
{
	struct stat buf;
	if (stat(file.c_str(), &buf) == -1) {
		dprintf(D_ALWAYS, "Unable to stat %s: (errno=%d, %s)\n", file.c_str(), errno, strerror(errno));
		return false;
	}

	std::string md5_str;
	Condor_MD_MAC md5;
	unsigned char* md5_raw;
	if (!md5.addMDFile(file.c_str())) {
		dprintf(D_ALWAYS, "Unable to md5sum the input file %s.\n", file.c_str());
		return false;
	} else if (!(md5_raw = md5.computeMD())) {
		dprintf(D_ALWAYS, "md5sum not available; cannot transfer file via http.\n");
		return false;
	} else {
		for (int i = 0; i < MAC_SIZE; i++) {
			sprintf_cat(md5_str, "%02x", static_cast<int>(md5_raw[i]));
		}
		free(md5_raw);
	}

	std::stringstream ss;
	ss << m_web_root << "/" << md5_str << "/" << file;
	std::string path_str(ss.str());

	// Note we don't update any internal structures until the IO operations are all finished.
	//
	classad_unordered<std::string, size_t>::iterator refcount_it = m_file_refcount.find(path_str);
	if (refcount_it != m_file_refcount.end()) {
		*refcount_it ++;
	} else {
		m_file_refcount[path_str] = 0;
		classad_unordered<std::string, off_t>::iterator usage_it = m_usage_stats.find(user);
		if (usage_it == m_usage_stats.end()) {
			m_usage_stats[user] = buf.st_size;
		} else {
			usage_it->second += buf.st_size;
		}
		m_file_to_request[file] = path_str;
		m_request_to_file[path_str] = file;
	}

	ss.str("");
	ss << "http://" << get_local_hostname().Value() << "/" << m_web_root << "/" << md5_str << "/" << file;
	resulting_name = ss.str();

	return true;
}

bool
TransferInputHttp::RemoveFile(const std::string &file, const std::string &user)
{
	classad_unordered<std::string, size_t>::iterator refcount = m_file_refcount.find(file);
	if ((refcount == m_file_refcount.end()) || ((refcount->second) != 1)) {
		m_file_refcount[file] = refcount->second - 1;
		return false;
	}
	m_file_refcount[file] = refcount->second - 1;

	// Decrease the usage stats.
	struct stat buf;
	if (stat(file.c_str(), &buf) == -1) {
		dprintf(D_ALWAYS, "Unable to stat %s: (errno=%d, %s)\n", file.c_str(), errno, strerror(errno));
		// Note we don't return false - we just cant reduce their usage stats.
	} else {
		classad_unordered<std::string, off_t>::iterator usage_it = m_usage_stats.find(user);
		if (usage_it != m_usage_stats.end()) {
			usage_it->second -= buf.st_size;
		}
	}

	// Remove the file from the maps.
	classad_unordered<std::string, std::string>::iterator req_it = m_file_to_request.find(file);
	if (req_it != m_file_to_request.end()) {
		m_request_to_file.erase(req_it->second);
	}
	m_file_to_request.erase(file);

	return true;
}

bool
TransferInputHttp::JobExitQueue(const compat_classad::ClassAd& jobad)
{
	std::string cache_files;
	if (!jobad.LookupString(ATTR_TRANSFER_CACHE_INPUT_FILES, cache_files)) {
		return false;
	}
	StringList cache_list(cache_files.c_str(), ",");
	cache_list.rewind();
	const char * next_file;

	std::string user;
	if (!jobad.LookupString(ATTR_OWNER, user)) {
		dprintf(D_ALWAYS, "No username associated with job.\n");
		return false;
	}

	while ( (next_file = cache_list.next()) ) {
		RemoveFile(next_file, user);
	}
	return true;
}

