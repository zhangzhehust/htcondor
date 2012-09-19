
#include "condor_common.h"
#include "string_list.h"
#include "condor_attributes.h"
#include "ipv6_hostname.h"
#include "condor_daemon_core.h"
#include "condor_config.h"

#include "stdsoap2.h"
#include <sstream>
#include <vector>
#include "boost/tokenizer.hpp"

#include "transfer_input.h"

extern DaemonCore *daemonCore;

TransferInputHttp* TransferInputHttp::g_input = NULL;

using namespace boost;

static bool
abspath_internal(const std::string & filename, std::string & result)
{
	char_delimiters_separator<char> sep(false, "", "/");
	tokenizer<> tok(filename, sep);
	std::vector<std::string> path_pieces;
	for(tokenizer<>::const_iterator it=tok.begin(); it!=tok.end(); it++) {
		if (*it == ".") {
			continue;
		} else if (*it == "..") {
			path_pieces.pop_back();
		} else {
			path_pieces.push_back(*it);
		}
	}
	std::stringstream ss;
	for (std::vector<std::string>::const_iterator it2 = path_pieces.begin(); it2 != path_pieces.end(); it2++) {
		ss << "/" << *it2;
	}
	result = ss.str();
	return true;
}

static bool
abspath(const std::string & filename, const ClassAd & ad, std::string & result)
{
	if (filename.empty())
		return false;
	result.clear();
	if (filename[0] != '/') {
		std::string iwd;
		if (!ad.LookupString(ATTR_JOB_IWD, iwd)) {
			return false;
		}
		return abspath_internal(iwd + "/" + filename, result);
	} else {
		return abspath_internal(filename, result);
	}
}

// TODO: How to best reap children?

int
TransferInputHttp::GetHandler(struct soap * gsoap) {
	TransferInputHttp & input = TransferInputHttp::GetInstance();

	// If its not a registered request, pass this to the prior Condor handler.	
	classad_unordered<std::string, std::string>::iterator req_it = input.m_request_to_file.find(gsoap->path);
	if (req_it == input.m_request_to_file.end()) {
		return input.m_condor_get_handler(gsoap);
	}

	gsoap->http_content = "application/octet-stream";

	// TODO: user privileges
	// init_user_ids_quiet(username);
	TemporaryPrivSentry sentry(PRIV_CONDOR);

	FILE *fstr = safe_fopen_wrapper(gsoap->path, "rb");
	if (fstr == NULL) {
		return 404; // TODO: check retval.
	}

	struct soap * gsoap_copy = soap_copy(gsoap);

	ForkStatus fork_status = FORK_FAILED;
	fork_status = input.m_transfer_forker.NewJob();
	if( fork_status == FORK_PARENT ) {
		// Close soap
		close(gsoap->socket);
		gsoap->socket = SOAP_INVALID_SOCKET;
		soap_end(gsoap);
		soap_free(gsoap);
		fclose(fstr);
		return SOAP_OK;
	}

	// Drop privs forever if we're the child.
	if (fork_status == FORK_CHILD) {
		set_priv(PRIV_CONDOR_FINAL);
	}

	// FORK_BUSY or FORK_CHILD
	int retval = ServeFile(fstr, gsoap_copy);

	if (fork_status == FORK_CHILD) {
		input.m_transfer_forker.WorkerDone();
	}

	soap_end(gsoap_copy);
	soap_done(gsoap_copy);

	return retval;
}

int
TransferInputHttp::ServeFile(FILE * file, struct soap * gsoap)
{
	char byte_buffer [4096];
	ssize_t bytes_read;
	while ((bytes_read = fread(byte_buffer, 1, sizeof(byte_buffer), file))) {
		if (soap_send_raw(gsoap, byte_buffer, bytes_read) != SOAP_OK) {
			dprintf(D_FULLDEBUG, "Failed to send data to remote host.\n");
			if (soap_end_send(gsoap) != SOAP_OK) {
				dprintf(D_FULLDEBUG, "Failed to end transaction\n");
				fclose(file);
				return gsoap->error;
			}
		}
	}
	if (ferror(file)) {
		dprintf(D_ALWAYS, "Failed to serve file: (errno=%d, %s)\n", errno, strerror(errno));
		fclose(file);
		gsoap->error = SOAP_HTTP_ERROR;
		return 500;
	}
	fclose(file);

	if (soap_end_send(gsoap) != SOAP_OK) {
		dprintf(D_FULLDEBUG, "Failed to end connection after file transfer.\n");
		return gsoap->error;
	}

	return SOAP_OK;
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
			dprintf(D_FULLDEBUG, "Keeping file %s on input files list.\n", next_file);
		} else {
			resulting_input_files.push_back(next_file);
			dprintf(D_FULLDEBUG, "Adding file %s to cache file list.\n", next_file);
		}
	}

	dprintf(D_FULLDEBUG, "Adding %lu files to the cache list.\n", resulting_cache_files.size());

	std::string user;
	if (!jobad.LookupString(ATTR_OWNER, user)) {
		dprintf(D_ALWAYS, "No username associated with job.\n");
		return false;
	}
	for (std::vector<std::string>::const_iterator it=resulting_cache_files.begin(); it != resulting_cache_files.end(); it++)
	{
		std::string fullpath;
		if (!abspath(*it, jobad, fullpath)) {
			dprintf(D_ALWAYS, "Improperly named cache file %s.\n", it->c_str());
			return false;
		}
		dprintf(D_FULLDEBUG, "Attempting to cache input file %s.\n", it->c_str());
		std::string resulting_name;
		if (AddFile(*it, user, resulting_name)) {
			// TODO: check for duplicates
			resulting_cache_files.push_back(resulting_name);
		} else {
			resulting_input_files.push_back(*it);
		}
	}

	if (!resulting_input_files.empty()) {
		// TODO: Do we need to add an expression to the classad noting we need a http handler?
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
	// TODO: Sanitize string
	// XXX:  Finish adding user hashing
	ss << m_web_root << "/" << md5_str << "/" << user << "/" << file;
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
	ss << "http://" << get_local_hostname().Value() << "/" << m_web_root << "/" << md5_str << "/" << user << "/" << file;
	resulting_name = ss.str();

	return true;
}

bool
TransferInputHttp::RemoveFile(const std::string &file, const std::string &user)
{
	classad_unordered<std::string, size_t>::iterator refcount = m_file_refcount.find(file);
	if (refcount == m_file_refcount.end()) {
		return false;
	}
	m_file_refcount[file] = refcount->second - 1;
	if (refcount->second - 1 != 0) {
		return false;
	}

	dprintf(D_FULLDEBUG, "Removing %s due to its last refcount decreasing.", file.c_str());
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

