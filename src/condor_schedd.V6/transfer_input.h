
#include <string>

#include "classad/classad_stl.h"

class TransferInputHttp {

public:

	// Reconfig the object.  Grab the soap structure from DaemonCore and
	// replace it with our own handler.
	//
	// Re-compute the usage statistics.
	Reconfig();

	// A job has all its inputs spooled and available.
	//
	// NOTE: this will update the classad to reflect the input URLs.
	int SpoolReady(ClassAd &job);

	// A job is ready to exit the queue; remove the job from the staging area.
	int JobExitQueue(const ClassAd &job);

	// Given a user, return the total usage (in bytes) of the cache.
	unsigned int GetUsage(const std::string & user);

	// Return an instance of the manager
	TransferInputHttp& GetInstance();

private:

	std::string web_root;
	classad_unordered<std::string, unsigned int> usage_stats;

};
