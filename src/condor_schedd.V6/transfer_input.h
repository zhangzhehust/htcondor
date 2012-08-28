
#ifndef __TRANSFER_INPUT_H_
#define __TRANSFER_INPUT_H_

#include <string>

#include "classad/classad_stl.h"
#include "compat_classad.h"
#include "forkwork.h"

class TransferInputHttp {

public:

	TransferInputHttp()
		:
			m_web_root("/cached_input_files"),
			m_condor_get_handler(NULL)
		{}

	// Reconfig the object.  Grab the soap structure from DaemonCore and
	// replace it with our own handler.
	//
	// Re-compute the usage statistics.
	void Reconfig();

	// A job has all its inputs spooled and available.
	//
	// If the input classad has changed and needs to be committed, return true.
	// Else, return false.
	bool SpoolReady(compat_classad::ClassAd &job);

	// A job is ready to exit the queue; remove the job from the staging area.
	bool JobExitQueue(const compat_classad::ClassAd &job);

	// Given a user, return the total usage (in bytes) of the cache.
	unsigned int GetUsage(const std::string & user);

	// Return an instance of the manager
	static TransferInputHttp& GetInstance();

private:

	const std::string m_web_root;
	classad_unordered<std::string, off_t> m_usage_stats;
	classad_unordered<std::string, size_t> m_file_refcount;
	classad_unordered<std::string, std::string> m_file_to_request;
	classad_unordered<std::string, std::string> m_request_to_file;

	ForkWork m_transfer_forker;

	static TransferInputHttp* g_input;

	bool AddFile(const std::string &file, const std::string &user, std::string &resulting_name);
	bool RemoveFile(const std::string &file, const std::string &user);

	static int GetHandler(struct soap *);

	int (*m_condor_get_handler)(struct soap *);

};

#endif

